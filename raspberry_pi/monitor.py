#!/usr/bin/env python3
"""
Minimal live demo: connects to the RayGlides EMS over USB Serial and
prints every decoded frame it receives. Requires real hardware plugged
in at the given port - this is NOT run as part of the automated test
suite (see test_rayglides_protocol.py for that).

Usage:
    python3 monitor.py                       # defaults to /dev/ttyACM0 @ 115200
    python3 monitor.py --port /dev/ttyUSB0
"""

import argparse
import sys

from rayglides_protocol import RayGlidesLink, decode_frame


def main():
    parser = argparse.ArgumentParser(description="Live RayGlides protocol monitor")
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    print(f"Connecting to {args.port} @ {args.baud} baud... (Ctrl+C to quit)")
    try:
        with RayGlidesLink(port=args.port, baudrate=args.baud) as link:
            while True:
                for frame in link.poll():
                    decoded = decode_frame(frame)
                    print(f"[{frame.msg_name}] {decoded}")
                if link.checksum_errors:
                    print(f"  (checksum errors so far: {link.checksum_errors})", file=sys.stderr)
    except KeyboardInterrupt:
        print("\nStopped.")
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        print("Check: is the board plugged in? Is the port right? "
              "Does your user have permission (try: sudo usermod -aG dialout $USER, then re-login)?",
              file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
