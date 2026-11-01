"""Stand-in for the two ElevenLabs endpoints leansipctl uses, for the loopback integration test.

Prints the port it listens on (127.0.0.1) as its first line, then serves until killed.
  POST /v1/text-to-speech/<voice>?output_format=pcm_16000  -> 1 s of a 1 kHz tone, 16 kHz PCM
  POST /v1/speech-to-text (multipart)                      -> a fixed transcript
Both require the header xi-api-key: test-key.
"""

import json
import math
import struct
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

TRANSCRIPT = "hello from the fake speech service"


def tone_pcm(seconds=1.0, rate=16000, hz=1000.0):
    return b"".join(
        struct.pack("<h", int(12000 * math.sin(2 * math.pi * hz * i / rate))) for i in range(int(seconds * rate))
    )


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.headers.get("xi-api-key") != "test-key":
            self.reply(401, b'{"detail":{"message":"Invalid API key"}}', "application/json")
        elif self.path.startswith("/v1/text-to-speech/") and "output_format=pcm_16000" in self.path:
            text = json.loads(body)["text"]
            self.reply(200, tone_pcm() if text else b"", "application/octet-stream")
        elif self.path == "/v1/speech-to-text" and b'name="model_id"' in body and b"RIFF" in body:
            answer = {"text": TRANSCRIPT, "language_code": "eng", "language_probability": 1.0, "words": []}
            self.reply(200, json.dumps(answer).encode(), "application/json")
        else:
            self.reply(400, b'{"detail":{"message":"unexpected request"}}', "application/json")


def main():
    server = HTTPServer(("127.0.0.1", 0), Handler)
    print(server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
