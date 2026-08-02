#include "pch.hpp"

#include "app/application.hpp"

#include "app/application_runtime.hpp"

namespace minimize::app {

Application::Application() : runtime_(std::make_unique<ApplicationRuntime>()) {}
Application::~Application() = default;

bool Application::Initialize(HINSTANCE instance, const ApplicationLaunchOptions& options) {
  return runtime_->Initialize(instance, options);
}

int Application::Run() { return runtime_->Run(); }

void Application::RenderUpdateHandoverFrame() { runtime_->RenderUpdateHandoverFrame(); }

bool Application::CompleteUpdateHandover() { return runtime_->CompleteUpdateHandover(); }

void Application::RequestShutdown() { runtime_->RequestShutdown(); }

}  // namespace minimize::app
