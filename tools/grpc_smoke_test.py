from pathlib import Path
import sys

import grpc


ROOT = Path(__file__).resolve().parents[1]
GENERATED_PY = ROOT / "generated" / "python"
if str(GENERATED_PY) not in sys.path:
    sys.path.insert(0, str(GENERATED_PY))

import jarvis_pb2
import jarvis_pb2_grpc


def call_command(stub, command, payload, label=None):
    # Build a request object
    request = jarvis_pb2.ExecuteCommandRequest(command=command, payload=payload)

    # GRPC serialises request and sends it over TCP to the server, which processes it and sends back a response.
    response = stub.ProcessCommand(request)

    if label:
        print(f"[{label}]")
    print("success:", response.success)
    print("message:", response.message)
    print("command_type:", jarvis_pb2.CommandType.Name(response.command_type))
    print("error_code:", jarvis_pb2.ErrorCode.Name(response.error_code))
    print("-" * 40)


def main():
    channel = grpc.insecure_channel("localhost:50051")

    # stub is pythons auto generated proxy object
    # Calling stub.ProcessCommand() feels like a local function call,
    # but under the hood it serialises data and sends it over TCP.
    stub = jarvis_pb2_grpc.JarvisServiceStub(channel)


    # Test various command types. The server will route these to the appropriate handler based on the command type.
    # Known command types dont need to go through the python ai layer, they should simply
    call_command(stub, jarvis_pb2.COMMAND_TYPE_ECHO, "hello from python")
    call_command(stub, jarvis_pb2.COMMAND_TYPE_HELP, "")
    call_command(stub, jarvis_pb2.COMMAND_TYPE_STATUS, "")

    # UNKNOWN commands route to the Python AI server for classification.
    # The server re-dispatches classified intents (STATUS/ECHO/ABOUT) as if the
    # matching known command had been sent directly — command_type in the
    # response reflects the classified command, not UNKNOWN.
    call_command(
        stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "how long have you been running",
        label="classified -> STATUS",
    )
    call_command(
        stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "say hello there",
        label="classified -> ECHO",
    )
    call_command(
        stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "who are you",
        label="classified -> ABOUT",
    )
    # Genuine fallback: nothing the rule classifier recognises, so command_type
    # stays UNKNOWN and message is the AI server's placeholder reply.
    call_command(
        stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "what is the meaning of life",
        label="unclassified -> UNKNOWN",
    )

if __name__ == "__main__":
    main()
