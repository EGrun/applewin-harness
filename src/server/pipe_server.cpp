#include "server/pipe_server.h"
#include <stdexcept>
#include <utility>

// Task 2 stubs: implemented in Task 4.

namespace harness {

struct PipeServer::Impl {
    std::wstring name;
    Tokens tokens;
    RequestHandler onRequest;
    MethodPredicate isOperatorOnly;
    DisplayMode display;
};

PipeServer::PipeServer(std::wstring name, Tokens tokens, RequestHandler onRequest, MethodPredicate isOperatorOnly,
                       DisplayMode display)
    : impl_(std::make_unique<Impl>(
          Impl{std::move(name), std::move(tokens), std::move(onRequest), std::move(isOperatorOnly), display}))
{
}

PipeServer::~PipeServer()
{
    Stop();
}

void PipeServer::Start()
{
    throw std::logic_error("PipeServer::Start: not implemented");
}

void PipeServer::Stop()
{
}

std::wstring PipeServer::RandomPipeName()
{
    throw std::logic_error("PipeServer::RandomPipeName: not implemented");
}

std::string PipeServer::RandomToken()
{
    throw std::logic_error("PipeServer::RandomToken: not implemented");
}

}  // namespace harness
