#!/usr/bin/env python3
import argparse
import shutil
import signal
import socket
import subprocess
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Bind a UDP port and advertise it via Avahi DNS-SD for Quest discovery.")
    parser.add_argument("--bind-host", default="0.0.0.0", help="UDP bind host. Default: 0.0.0.0")
    parser.add_argument("--port", type=int, default=5005, help="UDP port to bind and advertise. Default: 5005")
    parser.add_argument("--service-name", default="Quest Bridge", help="mDNS/DNS-SD service name.")
    parser.add_argument("--ros-domain-id", default="0", help="TXT ros_domain_id value.")
    parser.add_argument("--node", default="quest_bridge", help="TXT node value.")
    parser.add_argument("--caps", default="pose,buttons,hmd", help="TXT caps value.")
    parser.add_argument("--no-advertise", action="store_true", help="Skip Avahi publication and only bind the UDP socket.")
    return parser.parse_args()


def start_avahi_publisher(args: argparse.Namespace) -> subprocess.Popen[str] | None:
    if args.no_advertise:
        print("Skipping Avahi advertisement (--no-advertise).")
        return None

    avahi_binary = shutil.which("avahi-publish-service")
    if avahi_binary is None:
        print("avahi-publish-service not found; running UDP receiver without DNS-SD advertisement.", file=sys.stderr)
        return None

    command = [
        avahi_binary,
        args.service_name,
        "_quest-teleop._udp",
        str(args.port),
        "proto=1",
        f"ros_domain_id={args.ros_domain_id}",
        f"node={args.node}",
        f"caps={args.caps}",
    ]
    print("Advertising service:", " ".join(command))
    return subprocess.Popen(command)


def main() -> int:
    args = parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind_host, args.port))

    publisher = start_avahi_publisher(args)

    def shutdown_handler(signum, frame):
        del signum, frame
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    packet_count = 0
    print(f"UDP receiver listening on {args.bind_host}:{args.port}")
    try:
        while True:
            payload, address = sock.recvfrom(65535)
            packet_count += 1
            print(f"[{packet_count}] {address[0]}:{address[1]} {len(payload)} bytes")
    except KeyboardInterrupt:
        print("Stopping receiver.")
    finally:
        sock.close()
        if publisher is not None:
            publisher.terminate()
            try:
                publisher.wait(timeout=2)
            except subprocess.TimeoutExpired:
                publisher.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())