// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "config.h"

#include <wtf/HashSet.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>

#include "Document.h"
#include "Node.h"
#include "Page.h"
#undef LOG

#include "grit/webkit_resources.h"
#include "V8Binding.h"
#include "V8DOMWindow.h"
#include "v8_binding.h"
#include "v8_index.h"
#include "v8_proxy.h"
#include "webkit/glue/devtools/debugger_agent_impl.h"
#include "webkit/glue/devtools/debugger_agent_manager.h"
#include "webkit/glue/glue_util.h"
#include "webkit/glue/webdevtoolsagent_impl.h"
#include "webkit/glue/webkit_glue.h"
#include "webkit/glue/webview_impl.h"

using WebCore::DOMWindow;
using WebCore::Document;
using WebCore::Node;
using WebCore::Page;
using WebCore::String;
using WebCore::V8ClassIndex;
using WebCore::V8Custom;
using WebCore::V8DOMWindow;
using WebCore::V8Proxy;

DebuggerAgentImpl::DebuggerAgentImpl(
    WebViewImpl* web_view_impl,
    DebuggerAgentDelegate* delegate,
    WebDevToolsAgentImpl* webdevtools_agent)
    : web_view_impl_(web_view_impl),
      delegate_(delegate),
      webdevtools_agent_(webdevtools_agent) {
  DebuggerAgentManager::DebugAttach(this);
}

DebuggerAgentImpl::~DebuggerAgentImpl() {
  DebuggerAgentManager::DebugDetach(this);
}

void DebuggerAgentImpl::DebugBreak() {
  DebuggerAgentManager::DebugBreak(this);
}

void DebuggerAgentImpl::GetContextId() {
  delegate_->DidGetContextId(webdevtools_agent_->host_id());
}

void DebuggerAgentImpl::StartProfiling() {
  v8::V8::ResumeProfiler();
}

void DebuggerAgentImpl::StopProfiling() {
  v8::V8::PauseProfiler();
}

void DebuggerAgentImpl::GetLogLines(int position) {
  static char buffer[65536];
  int read_size = v8::V8::GetLogLines(position, buffer, sizeof(buffer) - 1);
  buffer[read_size] = '\0';
  DidGetLogLines(buffer, position + read_size);
}

void DebuggerAgentImpl::DebuggerOutput(const std::string& command) {
  delegate_->DebuggerOutput(command);
  webdevtools_agent_->ForceRepaint();
}

void DebuggerAgentImpl::DidGetLogLines(
    const std::string& log, int new_position) {
  delegate_->DidGetLogLines(log, new_position);
}

// static
void DebuggerAgentImpl::ResetUtilityContext(
    Document* document,
    v8::Persistent<v8::Context>* context) {
  if (!context->IsEmpty()) {
    context->Dispose();
    context->Clear();
  }
  if (!document) {
    return;
  }
  v8::HandleScope scope;

  // TODO(pfeldman): Validate against Soeren.
  // Set up the DOM window as the prototype of the new global object.
  v8::Handle<v8::Context> window_context =
      V8Proxy::GetContext(document->frame());
  v8::Handle<v8::Object> window_global = window_context->Global();
  v8::Handle<v8::Value> window_wrapper =
      V8Proxy::LookupDOMWrapper(V8ClassIndex::DOMWINDOW, window_global);

  ASSERT(V8Proxy::DOMWrapperToNative<DOMWindow>(window_wrapper) ==
      document->frame()->domWindow());

  // Create a new environment using an empty template for the shadow
  // object.  Reuse the global object if one has been created earlier.
  v8::Handle<v8::ObjectTemplate> global_template =
      V8DOMWindow::GetShadowObjectTemplate();

  // Install a security handler with V8.
  global_template->SetAccessCheckCallbacks(
      V8Custom::v8DOMWindowNamedSecurityCheck,
      V8Custom::v8DOMWindowIndexedSecurityCheck,
      v8::Integer::New(V8ClassIndex::DOMWINDOW));

  *context = v8::Context::New(
      NULL /* no extensions */,
      global_template,
      v8::Handle<v8::Object>());
  v8::Context::Scope context_scope(*context);
  v8::Handle<v8::Object> global = (*context)->Global();

  v8::Handle<v8::String> implicit_proto_string = v8::String::New("__proto__");
  global->Set(implicit_proto_string, window_wrapper);

  // Give the code running in the new context a way to get access to the
  // original context.
  global->Set(v8::String::New("contentWindow"), window_global);

  // Inject javascript into the context.
  StringPiece basejs = webkit_glue::GetDataResource(IDR_DEVTOOLS_BASE_JS);
  v8::Script::Compile(v8::String::New(basejs.as_string().c_str()))->Run();
  StringPiece injectjs = webkit_glue::GetDataResource(IDR_DEVTOOLS_INJECT_JS);
  v8::Script::Compile(v8::String::New(injectjs.as_string().c_str()))->Run();
  StringPiece inject_dispatchjs = webkit_glue::GetDataResource(
      IDR_DEVTOOLS_INJECT_DISPATCH_JS);
  v8::Script::Compile(v8::String::New(
      inject_dispatchjs.as_string().c_str()))->Run();
}

String DebuggerAgentImpl::ExecuteUtilityFunction(
    v8::Handle<v8::Context> context,
    const String &function_name,
    Node* node,
    const String& json_args,
    String* exception) {
  v8::HandleScope scope;
  ASSERT(!context.IsEmpty());
  v8::Context::Scope context_scope(context);
  v8::Handle<v8::Function> function = v8::Local<v8::Function>::Cast(
      context->Global()->Get(v8::String::New("devtools$$dispatch")));

  v8::Handle<v8::Value> node_wrapper =
      V8Proxy::ToV8Object(V8ClassIndex::NODE, node);
  v8::Handle<v8::String> function_name_wrapper = v8::Handle<v8::String>(
      v8::String::New(function_name.utf8().data()));
  v8::Handle<v8::String> json_args_wrapper = v8::Handle<v8::String>(
      v8::String::New(json_args.utf8().data()));
  v8::Handle<v8::Value> args[] = {
    function_name_wrapper,
    node_wrapper,
    json_args_wrapper
  };

  v8::TryCatch try_catch;
  v8::Handle<v8::Value> res_obj = function->Call(context->Global(), 3, args);
  if (try_catch.HasCaught()) {
    *exception = WebCore::ToWebCoreString(try_catch.Message()->Get());
    return "";
  } else {
    v8::Handle<v8::String> res_json = v8::Handle<v8::String>::Cast(res_obj);
    return WebCore::toWebCoreString(res_json);
  }
}

WebCore::Page* DebuggerAgentImpl::GetPage() {
  return web_view_impl_->page();
}
