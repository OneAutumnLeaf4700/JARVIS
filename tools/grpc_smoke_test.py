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
    # Build a request object
    request = jarvis_pb2.ExecuteCommandRequest(command=command, payload=payload)
    
    # GRPC serialises request and sends it over TCP to the server, which processes it and sends back a response.
    response = stub.ProcessCommand(request)

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
    # UNKNOWN routes to the Python AI server — payload is the full natural language text.
    call_command(stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "what is the weather like today?")

if __name__ == "__main__":
    main()
