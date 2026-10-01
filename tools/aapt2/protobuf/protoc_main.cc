// protoc for the host aapt2 (ADR-0039 b1) with the C++ generator only: aapt2's .proto files need nothing else.
// It stands in for protobuf's compiler/main.cc, which also registers the Java, Python, C#, Objective-C, PHP and
// Ruby generators and with them their sources.
#include <google/protobuf/compiler/command_line_interface.h>
#include <google/protobuf/compiler/cpp/generator.h>

int main(int argc, char* argv[]) {
  google::protobuf::compiler::CommandLineInterface cli;
  google::protobuf::compiler::cpp::CppGenerator cpp_generator;
  cli.RegisterGenerator("--cpp_out", "--cpp_opt", &cpp_generator, "Generate C++ header and source.");
  return cli.Run(argc, argv);
}
