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


class JarvisAIServicer(ai_pb2_grpc.JarvisAIServiceServicer):
    def ProcessNaturalLanguage(self, request, context):
        print(f"[AI] received: '{request.text}'")

        # Placeholder: echo the input back.
        # This will be replaced with a real LLM call in Phase 3.
        reply = f"[AI echo] {request.text}"

        print(f"[AI] replying: '{reply}'")
        return ai_pb2.NaturalLanguageResponse(success=True, reply=reply)


def serve():
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
    ai_pb2_grpc.add_JarvisAIServiceServicer_to_server(JarvisAIServicer(), server)
    server.add_insecure_port("[::]:50052")
    server.start()
    print("JARVIS AI server listening on port 50052")
    server.wait_for_termination()


if __name__ == "__main__":
    serve()
