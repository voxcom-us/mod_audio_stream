import asyncio
import base64
import json
from datetime import datetime
from pathlib import Path
from typing import List

import numpy as np
from fastapi import FastAPI, WebSocket
from loguru import logger
from scipy.io.wavfile import write
from starlette.websockets import WebSocketDisconnect

SR = 16000  # sample rate (Hz)

app = FastAPI()


class API:
    def __init__(self, out_dir: Path = Path("./recordings")):
        self.out_dir = out_dir
        self.out_dir.mkdir(parents=True, exist_ok=True)

    async def forward(self, path: str, ingress: WebSocket):
        """
        Receive PCM16LE frames from client, stream the same audio bytes back
        to the client (loopback), and persist a single WAV file on disconnect.
        """
        chunks: List[np.ndarray] = []

        def _make_filename() -> Path:
            safe = "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in (path or "session"))
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            return self.out_dir / f"{safe}_{ts}.wav"

        try:
            while True:
                samples_bytes = await ingress.receive_bytes()
                b64 = base64.b64encode(samples_bytes).decode("ascii")
                payload = {
                    "type": "streamAudio",
                    "data": {
                        "audioDataType": "raw",
                        "sampleRate": SR,
                        "audioData": b64,
                    },
                }
                await ingress.send_text(json.dumps(payload, separators=(",", ":")))
                samples_int16 = np.frombuffer(samples_bytes, dtype=np.int16)
                chunks.append(samples_int16.copy())
        except WebSocketDisconnect as e:
            logger.info(f"websocket disconnected: code={e.code}, reason={getattr(e, 'reason', '')}")
        except Exception as e:
            logger.warning(f"websocket error: {e}")
        finally:
            if chunks:
                audio = np.concatenate(chunks)
                outfile = _make_filename()
                try:
                    write(str(outfile), SR, audio)  # writes int16 WAV
                    logger.info(f"saved recording: {outfile} (samples={audio.size}, duration={audio.size / SR:.2f}s)")
                except Exception as e:
                    logger.error(f"failed to write WAV file: {e}")
            else:
                logger.info("no audio chunks received; nothing to save.")


api = API()


@app.websocket("/live/{path}")
async def live_wss(path: str, ingress: WebSocket):
    logger.debug(f"connecting: path={path}")
    await ingress.accept()
    ws_ingress = asyncio.create_task(api.forward(path=path, ingress=ingress))
    await asyncio.gather(ws_ingress)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("echo:app", host="0.0.0.0", port=8080, reload=False)
