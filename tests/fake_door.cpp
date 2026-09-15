#include "fake_door.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonValue>

namespace fake_door {

QVector<Call> calls;
bool admitted = false;

namespace {
QString jsonText(const QJsonObject& obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// Take the callback OUT of the record before running it. A reply commonly makes
// the next call, which appends to `calls` and may move the vector's storage
// underneath a reference into it.
//
// A REPLY TO A CALL THAT WAS NEVER MADE IS REPORTED, NOT ASSERTED. A drive that
// asserted here would abort the whole run on the first missing call, which is
// precisely the failure a regression produces — and the useful output then is
// the drive's own line saying which call is absent, not a stack trace.
void deliver(int i, const logos::web::ModuleCallResult& res)
{
    if (i < 0 || i >= calls.size()) {
        qWarning("fake_door: answered call #%d, which was never made", i);
        return;
    }
    if (calls[i].answered) {
        qWarning("fake_door: call #%d was answered twice", i);
        return;
    }
    calls[i].answered = true;
    const logos::web::ModuleCallCallback cb = calls[i].callback;
    if (cb)
        cb(res);
}
} // namespace

void answerJson(int i, const QJsonObject& reply)
{
    logos::web::ModuleCallResult res;
    res.ok = true;
    res.value = QJsonValue(jsonText(reply));
    deliver(i, res);
}

void failCall(int i, const QString& error)
{
    logos::web::ModuleCallResult res;
    res.ok = false;
    res.errorCode = QStringLiteral("METHOD_FAILED");
    res.error = error;
    deliver(i, res);
}

void reset()
{
    calls.clear();
    admitted = false;
}

} // namespace fake_door

// ── the door itself ──────────────────────────────────────────────────────────

namespace logos {
namespace web {

void callModuleAsync(const QString& module, const QString& method,
                     const QJsonArray& args, ModuleCallCallback callback)
{
    fake_door::calls.append(fake_door::Call{ module, method, args, callback, false });
}

bool canCallModules()
{
    return fake_door::admitted;
}

bool hostAdmitted()
{
    return fake_door::admitted;
}

} // namespace web
} // namespace logos
