import asyncio
import base64
import json
from datetime import datetime
from pathlib import Path
from typing import List

import numpy as np
from fastapi import FastAPI, WebSocket
from loguru import logger
from scipy.io.wavfile import read, write
from starlette.websockets import WebSocketDisconnect

SR = 16000  # sample rate (Hz)

app = FastAPI()

class API:
    def __init__(self, out_dir: Path = Path("./recordings"), greeting_wav: Path | None = None):
        self.out_dir = out_dir
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self.greeting_payload: str | None = None
        if greeting_wav:
            try:
                rate, data = read(str(greeting_wav))
                if data.dtype != np.int16:
                    if np.issubdtype(data.dtype, np.floating):
                        data = np.clip(data, -1.0, 1.0)
                        data = (data * np.iinfo(np.int16).max).astype(np.int16)
                    else:
                        data = np.clip(data, np.iinfo(np.int16).min, np.iinfo(np.int16).max).astype(np.int16)
                if data.ndim > 1:
                    data = data.reshape(-1)
                b64 = base64.b64encode(data.tobytes()).decode("ascii")
                payload = {
                    "type": "streamAudio",
                    "data": {
                        "audioDataType": "raw",
                        "sampleRate": int(rate),
                        "audioData": b64,
                    },
                }
                self.greeting_payload = json.dumps(payload, separators=(",", ":"))
            except Exception as e:
                logger.warning(f"failed to load greeting wav: {greeting_wav} ({e})")

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
            # Play greeting FIRST (sequentially) and discard incoming audio during playback
            if self.greeting_payload:
                try:
                    greeting_data = json.loads(self.greeting_payload)
                    sample_rate = greeting_data["data"]["sampleRate"]
                    b64_audio = greeting_data["data"]["audioData"]
                    audio_bytes = base64.b64decode(b64_audio)

                    # Calculate chunk size for 100ms of audio
                    samples_per_chunk = int(sample_rate * 0.100)
                    bytes_per_chunk = samples_per_chunk * 2
                    chunk_duration = 0.100  # seconds

                    offset = 0
                    chunk_count = 0

                    # Calculate total greeting duration
                    total_samples = len(audio_bytes) // 2
                    greeting_duration = total_samples / sample_rate

                    # Send greeting chunks and discard incoming audio simultaneously
                    while offset < len(audio_bytes):
                        chunk_bytes = audio_bytes[offset:offset + bytes_per_chunk]
                        chunk_b64 = base64.b64encode(chunk_bytes).decode("ascii")

                        chunk_payload = {
                            "type": "streamAudio",
                            "data": {
                                "audioDataType": "raw",
                                "sampleRate": sample_rate,
                                "audioData": chunk_b64,
                            },
                        }

                        await ingress.send_text(json.dumps(chunk_payload, separators=(",", ":")))
                        chunk_count += 1
                        offset += bytes_per_chunk

                        # Small delay to pace chunks
                        await asyncio.sleep(chunk_duration * 0.9)

                    logger.debug(f"greeting sent in {chunk_count} chunks (~{greeting_duration:.2f}s)")

                    # Drain any queued incoming audio during greeting playback
                    # Use a short timeout to clear the queue without blocking too long
                    drain_start = asyncio.get_event_loop().time()
                    drain_count = 0
                    while asyncio.get_event_loop().time() - drain_start < 0.5:
                        try:
                            _ = await asyncio.wait_for(ingress.receive_bytes(), timeout=0.05)
                            drain_count += 1
                        except asyncio.TimeoutError:
                            break

                    if drain_count > 0:
                        logger.debug(f"drained {drain_count} queued audio packets after greeting")

                except Exception as e:
                    logger.warning(f"failed to send greeting: {e}")

            # Now start the normal echo loop
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


api = API(greeting_wav=Path("ivr-echo_your_audio_back-16k.wav"))


@app.websocket("/live/{path}")
async def live_wss(path: str, ingress: WebSocket):
    logger.debug(f"connecting: path={path}")
    await ingress.accept()
    ws_ingress = asyncio.create_task(api.forward(path=path, ingress=ingress))
    await asyncio.gather(ws_ingress)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("echo:app", host="0.0.0.0", port=8080, reload=False)
