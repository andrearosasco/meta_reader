import collections
import json
from argparse import Namespace
from unittest import mock
import unittest

from metareader import receiver


def make_payload() -> bytes:
    packet = {
        "sequence": 42,
        "transport": "wired",
        "tracking_valid": True,
        "display_time_ns": 123456789,
        "head_pose": {
            "valid": True,
            "position": [0.1, 0.2, 0.3],
            "orientation": [0.0, 0.0, 0.0, 1.0],
        },
        "hands": {
            "left": {
                "tracked": True,
                "wrist": {
                    "tracked": True,
                    "pose": {
                        "position": [1.0, 2.0, 3.0],
                        "orientation": [0.0, 0.0, 0.0, 1.0],
                    },
                },
                "palm": {
                    "tracked": True,
                    "pose": {
                        "position": [4.0, 5.0, 6.0],
                        "orientation": [0.0, 0.0, 0.0, 1.0],
                    },
                },
                "fingertips": {
                    "index_tip": {
                        "tracked": True,
                        "pose": {
                            "position": [7.0, 8.0, 9.0],
                            "orientation": [0.0, 0.0, 0.0, 1.0],
                        },
                        "radius": 0.012,
                    },
                    "thumb_tip": {
                        "tracked": False,
                    },
                },
            },
            "right": {
                "tracked": False,
                "wrist": {"tracked": False},
                "palm": {"tracked": False},
                "fingertips": {},
            },
        },
        "status": {
            "connection_state": "WIRED CONNECTED",
            "target_host": "127.0.0.1:5005",
        },
    }
    return json.dumps(packet).encode("utf-8")


class PacketDecoderTests(unittest.TestCase):
    def test_feed_reassembles_packets_across_chunks(self) -> None:
        decoder = receiver.PacketDecoder()
        payload_one = b"alpha"
        payload_two = b"beta"
        framed = (
            len(payload_one).to_bytes(4, byteorder="big")
            + payload_one
            + len(payload_two).to_bytes(4, byteorder="big")
            + payload_two
        )

        self.assertEqual(decoder.feed(framed[:6]), [])
        self.assertEqual(decoder.feed(framed[6:11]), [payload_one])
        self.assertEqual(decoder.feed(framed[11:]), [payload_two])


class ParseFrameTests(unittest.TestCase):
    def test_parse_frame_marks_tracked_joint_poses_valid(self) -> None:
        frame = receiver.parse_frame(make_payload())

        self.assertEqual(frame.sequence, 42)
        self.assertEqual(frame.transport, "wired")
        self.assertTrue(frame.head_pose.valid)
        self.assertTrue(frame.left_hand.tracked)
        self.assertTrue(frame.left_hand.wrist.tracked)
        self.assertTrue(frame.left_hand.wrist.pose.valid)
        self.assertEqual(frame.left_hand.wrist.pose.position, (1.0, 2.0, 3.0))
        self.assertTrue(frame.left_hand.palm.tracked)
        self.assertTrue(frame.left_hand.palm.pose.valid)
        self.assertEqual(frame.left_hand.palm.pose.position, (4.0, 5.0, 6.0))
        self.assertTrue(frame.left_hand.fingertips["index_tip"].tracked)
        self.assertTrue(frame.left_hand.fingertips["index_tip"].pose.valid)
        self.assertFalse(frame.left_hand.fingertips["thumb_tip"].tracked)
        self.assertIsNone(frame.left_hand.fingertips["thumb_tip"].pose)

    def test_describe_frame_lists_only_tracked_fingertips(self) -> None:
        frame = receiver.parse_frame(make_payload())

        description = receiver.describe_frame(frame)

        self.assertIn("left_hand=True[index_tip]", description)
        self.assertIn("right_hand=False[none]", description)

    def test_parse_frame_defaults_missing_optional_fields(self) -> None:
        frame = receiver.parse_frame(json.dumps({}).encode("utf-8"))

        self.assertEqual(frame.sequence, 0)
        self.assertEqual(frame.transport, "unknown")
        self.assertFalse(frame.tracking_valid)
        self.assertEqual(frame.head_pose.position, (0.0, 0.0, 0.0))
        self.assertFalse(frame.left_hand.tracked)
        self.assertEqual(frame.status.connection_state, "")


class ParseArgsTests(unittest.TestCase):
    def test_parse_args_defaults(self) -> None:
        args = receiver.parse_args([])

        self.assertEqual(args.bind_host, "0.0.0.0")
        self.assertEqual(args.port, 5005)
        self.assertEqual(args.tcp_port, 5005)
        self.assertFalse(args.no_advertise)
        self.assertFalse(args.no_auto_adb_reverse)

    def test_parse_args_accepts_overrides(self) -> None:
        args = receiver.parse_args([
            "--bind-host", "127.0.0.1",
            "--port", "6000",
            "--tcp-port", "7000",
            "--no-advertise",
            "--no-auto-adb-reverse",
        ])

        self.assertEqual(args.bind_host, "127.0.0.1")
        self.assertEqual(args.port, 6000)
        self.assertEqual(args.tcp_port, 7000)
        self.assertTrue(args.no_advertise)
        self.assertTrue(args.no_auto_adb_reverse)


class ModeSelectionTests(unittest.TestCase):
    def make_reader(self) -> receiver.MetaReader:
        reader_obj = object.__new__(receiver.MetaReader)
        reader_obj.args = Namespace(
            bind_host="0.0.0.0",
            port=5005,
            tcp_port=5005,
            service_name="Quest Bridge",
            ros_domain_id="0",
            node="quest_bridge",
            caps="pose,hands,fingertips,hmd",
            no_advertise=False,
            no_auto_adb_reverse=False,
        )
        reader_obj.adb_binary = "/usr/bin/adb"
        return reader_obj

    def test_select_mode_prefers_wired_when_adb_present(self) -> None:
        reader_obj = self.make_reader()

        with mock.patch.object(receiver, "_detect_adb_device", return_value=True):
            mode = receiver.MetaReader._select_mode(reader_obj)

        self.assertEqual(mode.name, "wired")
        self.assertIn("adb reverse", mode.detail)

    def test_select_mode_uses_wireless_without_adb(self) -> None:
        reader_obj = self.make_reader()

        with mock.patch.object(receiver, "_detect_adb_device", return_value=False):
            mode = receiver.MetaReader._select_mode(reader_obj)

        self.assertEqual(mode.name, "wireless")
        self.assertIn("udp:5005", mode.detail)


class ConfigureTransportTests(unittest.TestCase):
    class SelectorStub:
        def __init__(self) -> None:
            self.registrations: list[tuple[object, object, object]] = []

        def register(self, fileobj, events, data) -> None:
            self.registrations.append((fileobj, events, data))

    def make_reader(self, mode_name: str) -> receiver.MetaReader:
        reader_obj = object.__new__(receiver.MetaReader)
        reader_obj.args = Namespace(
            bind_host="0.0.0.0",
            port=5005,
            tcp_port=5006,
            service_name="Quest Bridge",
            ros_domain_id="0",
            node="quest_bridge",
            caps="pose,hands,fingertips,hmd",
            no_advertise=False,
            no_auto_adb_reverse=False,
        )
        reader_obj.mode = receiver.ReceiverMode(name=mode_name, detail="detail")
        reader_obj.selector = self.SelectorStub()
        reader_obj.adb_binary = "/usr/bin/adb"
        reader_obj.reverse_configured = False
        reader_obj.publisher = None
        return reader_obj

    def test_configure_transport_wired_registers_tcp_listener(self) -> None:
        reader_obj = self.make_reader("wired")
        tcp_socket = object()

        with mock.patch.object(receiver, "_configure_adb_reverse") as configure_reverse, \
             mock.patch.object(receiver, "_make_tcp_socket", return_value=tcp_socket):
            receiver.MetaReader._configure_transport(reader_obj)

        configure_reverse.assert_called_once_with("/usr/bin/adb", 5005, 5006)
        self.assertTrue(reader_obj.reverse_configured)
        self.assertEqual(reader_obj.selector.registrations, [(tcp_socket, mock.ANY, ("tcp-listener", None))])

    def test_configure_transport_wireless_registers_udp_listener_and_publisher(self) -> None:
        reader_obj = self.make_reader("wireless")
        udp_socket = object()
        publisher = object()

        with mock.patch.object(receiver, "_make_udp_socket", return_value=udp_socket), \
             mock.patch.object(receiver, "_start_avahi_publisher", return_value=publisher):
            receiver.MetaReader._configure_transport(reader_obj)

        self.assertFalse(reader_obj.reverse_configured)
        self.assertIs(reader_obj.publisher, publisher)
        self.assertEqual(len(reader_obj.selector.registrations), 1)
        fileobj, _events, data = reader_obj.selector.registrations[0]
        self.assertIs(fileobj, udp_socket)
        self.assertEqual(data[0], "udp")
        self.assertIsInstance(data[1], receiver.PacketDecoder)


class MetaReaderQueueTests(unittest.TestCase):
    def test_read_latest_drops_stale_pending_frames(self) -> None:
        class SelectorStub:
            @staticmethod
            def select(timeout=None):
                del timeout
                return []

        reader_obj = object.__new__(receiver.MetaReader)
        reader_obj.pending_frames = collections.deque([
            receiver.TelemetryFrame(1, "wired", True, 1, receiver.Pose(valid=True), receiver.HandState(), receiver.HandState(), receiver.StatusState(), {}),
            receiver.TelemetryFrame(2, "wired", True, 2, receiver.Pose(valid=True), receiver.HandState(), receiver.HandState(), receiver.StatusState(), {}),
            receiver.TelemetryFrame(3, "wired", True, 3, receiver.Pose(valid=True), receiver.HandState(), receiver.HandState(), receiver.StatusState(), {}),
        ])
        reader_obj.selector = SelectorStub()

        latest = receiver.MetaReader.read_latest(reader_obj, timeout=0.0)

        self.assertEqual(latest.sequence, 3)
        self.assertEqual(list(reader_obj.pending_frames), [])


if __name__ == "__main__":
    unittest.main()