#pragma once
// THE DOOR, HELD OPEN BY THE TEST — a stand-in for logos::web::callModuleAsync
// that records what the `web` backend asked for and lets the drive answer it.
//
// WHY A STUB AND NOT THE REAL THING. The door's only implementation is
// logos-module-builder's wasm/logos_view_wasm_host.cpp, which links into a
// Qt-wasm image and reaches the container through the page's MessagePort. There
// is no page here and no container: this check compiles the backend's own
// translation unit natively against a door that answers from the drive, which
// is what makes "which module does this variant ask, with what, in what order"
// assertable without a phone.
//
// WHAT IT DELIBERATELY KEEPS from the real door: a call is RECORDED, never
// answered inline, so the backend cannot be written as if a reply arrived
// before the call returned; and replies are given BY INDEX, so a sequence the
// backend chains in its callbacks is asserted as a sequence rather than as a
// set.
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include "logos_web_module_call.h"

namespace fake_door {

struct Call {
    QString module;
    QString method;
    QJsonArray args;
    logos::web::ModuleCallCallback callback;
    bool answered = false;
};

// Every call the backend has made, oldest first. Never cleared by an answer:
// the drive asserts on the whole sequence.
extern QVector<Call> calls;

// What canCallModules()/hostAdmitted() report. False by default so the
// backend's startup poll never fires a call the drive did not ask for.
extern bool admitted;

// Answer call `i` the way a rust-first module on this wire does: the reply is a
// JSON TEXT, and the envelope is the module's own `{ok, …}`.
void answerJson(int i, const QJsonObject& reply);
// Answer with a bare value — eth_rpc's `set_chain_config` shape.
void answerValue(int i, const QJsonValue& value);
// The call never reached the module: the transport's own failure, which is a
// different outcome from a module that said no.
void failCall(int i, const QString& error);

void reset();

} // namespace fake_door
