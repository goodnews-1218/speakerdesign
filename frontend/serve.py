"""Serve only this frontend on loopback; no device or model is accessed."""
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import argparse


class FrontendHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(Path(__file__).resolve().parent / "dist"), **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Start the Connect Studio local frontend.")
    parser.add_argument("--port", type=int, default=4173)
    parser.add_argument("--open", action="store_true", help="Open the local page in your default browser")
    args = parser.parse_args()
    with ThreadingHTTPServer(("127.0.0.1", args.port), FrontendHandler) as server:
        print(f"Connect Studio: http://127.0.0.1:{args.port}", flush=True)
        if args.open:
            import webbrowser
            webbrowser.open(f"http://127.0.0.1:{args.port}")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
