from pathlib import Path
import sys

import grpc


ROOT = Path(__file__).resolve().parents[1]
GENERATED_PY = ROOT / "generated" / "python"
if str(GENERATED_PY) not in sys.path:
    sys.path.insert(0, str(GENERATED_PY))

import jarvis_pb2
import jarvis_pb2_grpc


def call_command(stub, command, payload):
    request = jarvis_pb2.ExecuteCommandRequest(command=command, payload=payload)
    response = stub.ProcessCommand(request)
    print("success:", response.success)
    print("message:", response.message)
    print("command_type:", jarvis_pb2.CommandType.Name(response.command_type))
    print("error_code:", jarvis_pb2.ErrorCode.Name(response.error_code))
    print("-" * 40)


def main():
    channel = grpc.insecure_channel("localhost:50051")
    stub = jarvis_pb2_grpc.JarvisServiceStub(channel)

    call_command(stub, jarvis_pb2.COMMAND_TYPE_ECHO, "hello from python")
    call_command(stub, jarvis_pb2.COMMAND_TYPE_HELP, "")
    call_command(stub, jarvis_pb2.COMMAND_TYPE_STATUS, "")
    call_command(stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "this should fail")
    



if __name__ == "__main__":
    main()
