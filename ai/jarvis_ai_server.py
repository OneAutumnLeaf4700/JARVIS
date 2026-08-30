import logging
import time
from pathlib import Path
import sys

from concurrent import futures
import grpc

ROOT = Path(__file__).resolve().parents[1]
GENERATED_PY = ROOT / "generated" / "python"
if str(GENERATED_PY) not in sys.path:
    sys.path.insert(0, str(GENERATED_PY))

import ai_pb2
import ai_pb2_grpc

from resolver import resolve

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
logger = logging.getLogger("jarvis_ai_server")


class JarvisAIServicer(ai_pb2_grpc.JarvisAIServiceServicer):
    def ProcessNaturalLanguage(self, request, context):
        start = time.monotonic()
        intent, confidence, tier = resolve(request.text)
        latency_ms = (time.monotonic() - start) * 1000

        logger.info(
            "text=%r tier=%s intent=%s confidence=%.2f latency_ms=%.2f",
            request.text, tier, intent, confidence, latency_ms,
        )

        reply = f"[detected intent: {intent}, confidence {confidence:.2f}]"
        return ai_pb2.NaturalLanguageResponse(
            success=True,
            reply=reply,
            intent=intent,
            confidence=confidence,
        )


def serve():
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
    ai_pb2_grpc.add_JarvisAIServiceServicer_to_server(JarvisAIServicer(), server)
    server.add_insecure_port("[::]:50052")
    server.start()
    logger.info("JARVIS AI server listening on port 50052")
    server.wait_for_termination()


if __name__ == "__main__":
    serve()
