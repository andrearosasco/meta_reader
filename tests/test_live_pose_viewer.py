import importlib.util
import os
import pathlib
import unittest

from metareader import HandState, JointState, Pose


def load_viewer_module():
    os.environ.setdefault("METAREADER_VIEWER_BOOTSTRAPPED", "1")
    os.environ.setdefault("MPLBACKEND", "Agg")
    module_path = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "live_pose_viewer.py"
    spec = importlib.util.spec_from_file_location("live_pose_viewer_test", module_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


viewer = load_viewer_module()


class ViewerHelperTests(unittest.TestCase):
    def test_hand_reference_pose_prefers_palm_over_wrist(self) -> None:
        palm_pose = Pose(position=(1.0, 1.0, 1.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True)
        wrist_pose = Pose(position=(2.0, 2.0, 2.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True)
        hand = HandState(
            tracked=True,
            wrist=JointState(tracked=True, pose=wrist_pose),
            palm=JointState(tracked=True, pose=palm_pose),
        )

        pose, source = viewer.hand_reference_pose(hand)

        self.assertEqual(source, "palm")
        self.assertEqual(pose.position, palm_pose.position)

    def test_hand_reference_pose_falls_back_to_wrist(self) -> None:
        wrist_pose = Pose(position=(2.0, 2.0, 2.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True)
        hand = HandState(
            tracked=True,
            wrist=JointState(tracked=True, pose=wrist_pose),
            palm=JointState(tracked=False, pose=None),
        )

        pose, source = viewer.hand_reference_pose(hand)

        self.assertEqual(source, "wrist")
        self.assertEqual(pose.position, wrist_pose.position)

    def test_format_joint_position_returns_none_for_missing_joint(self) -> None:
        hand = HandState(tracked=True)

        self.assertEqual(viewer.format_joint_position(hand, "palm"), "none")

    def test_format_joint_position_formats_xyz(self) -> None:
        hand = HandState(
            tracked=True,
            palm=JointState(
                tracked=True,
                pose=Pose(position=(1.23456, -2.0, 3.125), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
            ),
        )

        self.assertEqual(viewer.format_joint_position(hand, "palm"), "(1.235, -2.000, 3.125)")

    def test_expanded_radius_includes_all_visible_positions(self) -> None:
        radius = viewer.expanded_radius(0.45, [(0.1, 0.2, 0.3), (0.8, -0.1, 0.2)])

        self.assertGreaterEqual(radius, 0.92)

    def test_synthesize_hand_pose_from_fingertips_uses_tracked_points(self) -> None:
        hand = HandState(
            tracked=True,
            wrist=JointState(
                tracked=True,
                pose=Pose(position=(0.0, 0.0, 0.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
            ),
            fingertips={
                "index_tip": JointState(
                    tracked=True,
                    pose=Pose(position=(1.0, 0.0, 0.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
                ),
                "middle_tip": JointState(
                    tracked=True,
                    pose=Pose(position=(0.0, 1.0, 0.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
                ),
                "little_tip": JointState(
                    tracked=True,
                    pose=Pose(position=(-1.0, 0.0, 0.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
                ),
            },
        )

        pose = viewer.synthesize_hand_pose_from_fingertips(hand)

        self.assertIsNotNone(pose)
        self.assertTrue(pose.valid)

    def test_hand_reference_pose_falls_back_to_fingertips(self) -> None:
        hand = HandState(
            tracked=True,
            fingertips={
                "index_tip": JointState(
                    tracked=True,
                    pose=Pose(position=(1.0, 0.0, 0.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
                ),
                "middle_tip": JointState(
                    tracked=True,
                    pose=Pose(position=(0.0, 1.0, 0.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
                ),
                "little_tip": JointState(
                    tracked=True,
                    pose=Pose(position=(-1.0, 0.0, 0.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
                ),
            },
        )

        pose, source = viewer.hand_reference_pose(hand)

        self.assertEqual(source, "fingertips")
        self.assertIsNotNone(pose)
        self.assertTrue(pose.valid)

    def test_tracked_fingertip_positions_returns_only_tracked_points(self) -> None:
        hand = HandState(
            tracked=True,
            fingertips={
                "index_tip": JointState(
                    tracked=True,
                    pose=Pose(position=(1.0, 2.0, 3.0), orientation=(0.0, 0.0, 0.0, 1.0), valid=True),
                ),
                "thumb_tip": JointState(tracked=False, pose=None),
            },
        )

        self.assertEqual(viewer.tracked_fingertip_positions(hand), [(1.0, 2.0, 3.0)])

    def test_expanded_radius_respects_minimum_when_positions_are_small(self) -> None:
        self.assertEqual(viewer.expanded_radius(0.45, [(0.1, 0.1, 0.1)]), 0.45)

    def test_quaternion_to_rotation_matrix_identity(self) -> None:
        matrix = viewer.quaternion_to_rotation_matrix((0.0, 0.0, 0.0, 1.0))

        self.assertEqual(matrix, ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)))


if __name__ == "__main__":
    unittest.main()