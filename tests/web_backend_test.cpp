// WHAT THE `web` VARIANT ASKS FOR, AND OF WHOM.
//
// This variant's whole contract is a sequence of calls to modules it names:
// accounts and keys out of `keystore_module`, balances out of `eth_rpc_module`,
// prices out of `uniswap_module`, and a REFUSAL BY NAME for everything only the
// coordinator can serve. None of that is visible in a build, and the bug this
// check landed with is exactly the kind a build cannot see: `importMnemonic`
// answered "Mnemonic import needs keystore_module, which has no mobile build"
// while the keystore was loaded, admitted and answering `list_accounts` in the
// same run (logos-workspace#147). The message was not a symptom of a broken
// call — no call was ever made.
//
// So what is asserted here is the ASKING: which module, which method, with what
// arguments, in what order — and, for the methods this variant really cannot
// serve, that the refusal still names the module that would have served it.
//
// HOW, without a phone: `tests/fake_door.cpp` implements the one door out
// (logos::web::callModuleAsync) as a recorder, so the backend's own translation
// unit is compiled natively and driven from here. The header there says what
// that stub keeps of the real door and why.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <optional>

#include "fake_door.h"
#include "wallet_ui_web_backend.h"

namespace {

int failures = 0;

void fail(const QString& why)
{
    std::fprintf(stderr, "FAIL: %s\n", qPrintable(why));
    ++failures;
}

void check(bool ok, const QString& why)
{
    if (!ok)
        fail(why);
}

void pass(const QString& what)
{
    std::printf("PASS: %s\n", qPrintable(what));
}

// LET A QUEUED RETRY FIRE. The backend retries a refusal from the door on a
// TIMER, because the thing it is waiting for — the core finishing this module's
// registration — takes wall time and not another turn of the loop. So a drive
// that wants to see the retry has to spend that wall time; it stops as soon as
// the retry has been made, so the cost is the delay and not the deadline.
void settleUntilCall(int index, int deadlineMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < deadlineMs && fake_door::calls.size() <= index)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

QJsonObject parse(const QString& text)
{
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

QString describe(const fake_door::Call& c)
{
    return QStringLiteral("%1.%2(%3)")
        .arg(c.module, c.method,
             QString::fromUtf8(QJsonDocument(c.args).toJson(QJsonDocument::Compact)));
}

// Call `i` was made, and it was this one. Answers an optional so a case can STOP
// at the first call that is missing or wrong: every later assertion in a drive
// that chains replies would otherwise fail for the same one reason, and the
// line that matters would be the first of a dozen.
//
// A COPY, NOT A REFERENCE INTO `fake_door::calls`: answering a call commonly
// makes the next one, which appends to that vector and may move its storage —
// the same hazard fake_door.cpp's `deliver` takes the callback out of the record
// for.
std::optional<fake_door::Call> expectCall(int i, const QString& module, const QString& method)
{
    if (i >= fake_door::calls.size()) {
        fail(QStringLiteral("no call #%1 was made; expected %2.%3, and the backend made "
                            "%4 call(s) in all")
                 .arg(i)
                 .arg(module, method)
                 .arg(fake_door::calls.size()));
        return std::nullopt;
    }
    const fake_door::Call c = fake_door::calls.at(i);
    if (c.module != module || c.method != method) {
        fail(QStringLiteral("call #%1 was %2; expected %3.%4")
                 .arg(i)
                 .arg(describe(c), module, method));
        return std::nullopt;
    }
    return c;
}

// The two calls every Tier D mutation on the keystore is preceded by: "who am I
// here?" and "may I mutate?". Answered as the module does, so the chain runs on.
bool admitTheCustodian(int identityCall)
{
    if (!expectCall(identityCall, QStringLiteral("keystore_module"),
                    QStringLiteral("caller_identity")))
        return false;
    fake_door::answerJson(identityCall,
                          QJsonObject{ { "kind", "module" }, { "identity", "wallet_ui" } });

    if (!expectCall(identityCall + 1, QStringLiteral("keystore_module"),
                    QStringLiteral("configure")))
        return false;
    fake_door::answerJson(identityCall + 1,
                          QJsonObject{ { "ok", true },
                                       { "custodians", QJsonArray{ "evm_keystore_ui", "wallet_ui" } } });
    return true;
}

const char* kPhrase = "test test test test test test test test test test test junk";

QString phraseJson()
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{ { "phrase", QString::fromUtf8(kPhrase) },
                                                        { "accountIndex", 0 },
                                                        { "password", "pw" } })
                                 .toJson(QJsonDocument::Compact));
}

// ── the import reaches the keystore ──────────────────────────────────────────
void importGoesToTheKeystore()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const QString taken = backend.importMnemonic(phraseJson(), QStringLiteral("main"));
    const QJsonObject answer = parse(taken);

    // #147: this answered `{"ok":false,"error":"Mnemonic import needs
    // keystore_module, which has no mobile build …"}` with no call made.
    check(answer.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("importMnemonic refused instead of taking the ask: %1").arg(taken));
    check(!backend.statusText().contains(QStringLiteral("no mobile build")),
          QStringLiteral("the status line still says the keystore has no mobile build: %1")
              .arg(backend.statusText()));

    if (!admitTheCustodian(0))
        return;

    const std::optional<fake_door::Call> imported =
        expectCall(2, QStringLiteral("keystore_module"), QStringLiteral("import_mnemonic"));
    if (!imported)
        return;
    // The phrase and the password reach the keystore as ONE params document —
    // the shape `import_mnemonic` parses — not as positional arguments.
    const QJsonObject params = parse(imported->args.isEmpty() ? QString()
                                                              : imported->args.at(0).toString());
    check(params.value(QStringLiteral("phrase")).toString() == QString::fromUtf8(kPhrase),
          QStringLiteral("the phrase did not reach the keystore: %1").arg(describe(*imported)));
    check(params.value(QStringLiteral("password")).toString() == QStringLiteral("pw"),
          QStringLiteral("the password did not reach the keystore: %1").arg(describe(*imported)));
    check(params.value(QStringLiteral("accountIndex")).toInt(-1) == 0,
          QStringLiteral("the derivation index did not reach the keystore: %1")
              .arg(describe(*imported)));

    fake_door::answerJson(2, QJsonObject{ { "ok", true },
                                          { "address", "0x85d7…40E9" },
                                          { "path", "m/44'/60'/0'/0/0" },
                                          { "origin", "derived" } });

    // The LABEL is the user's other field on that form, and the keystore is the
    // only place this variant can put it — there is no coordinator here to keep
    // a labels.json of its own.
    const std::optional<fake_door::Call> labelled =
        expectCall(3, QStringLiteral("keystore_module"), QStringLiteral("set_label"));
    if (!labelled)
        return;
    check(labelled->args.size() == 3
              && labelled->args.at(0).toString() == QStringLiteral("0x85d7…40E9")
              && labelled->args.at(1).toString() == QStringLiteral("main")
              && labelled->args.at(2).toString() == QStringLiteral("pw"),
          QStringLiteral("the label was not set on the imported account: %1")
              .arg(describe(*labelled)));
    fake_door::answerJson(3, QJsonObject{ { "ok", true } });

    // ...and only then is the list re-read, so what the view repaints includes
    // the account that was just imported.
    expectCall(4, QStringLiteral("keystore_module"), QStringLiteral("list_accounts"));
    check(backend.statusText() == QStringLiteral("Account imported"),
          QStringLiteral("status after a successful import: %1").arg(backend.statusText()));

    if (failures == before)
        pass("a mnemonic import is a Tier D mutation on keystore_module, in order");
}

// ── a refusal says what the keystore said ────────────────────────────────────
void aKeystoreRefusalIsReportedInItsOwnWords()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.importMnemonic(phraseJson(), QString());
    if (!admitTheCustodian(0)
        || !expectCall(2, QStringLiteral("keystore_module"), QStringLiteral("import_mnemonic")))
        return;
    fake_door::answerJson(2, QJsonObject{ { "ok", false }, { "error", "invalid checksum" } });

    check(backend.statusText().contains(QStringLiteral("invalid checksum")),
          QStringLiteral("the keystore's own reason did not reach the view: %1")
              .arg(backend.statusText()));
    check(!backend.statusText().contains(QStringLiteral("no mobile build")),
          QStringLiteral("a refused import is reported as a missing build: %1")
              .arg(backend.statusText()));
    // Nothing follows a refusal: no label to set and no new account to list.
    check(fake_door::calls.size() == 3,
          QStringLiteral("a refused import went on calling: %1 calls").arg(fake_door::calls.size()));

    if (failures == before)
        pass("a refused import reports the keystore's reason, and stops there");
}

// ── an empty label is not a call ─────────────────────────────────────────────
void withoutALabelNothingIsLabelled()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.importMnemonic(phraseJson(), QString());
    if (!admitTheCustodian(0)
        || !expectCall(2, QStringLiteral("keystore_module"), QStringLiteral("import_mnemonic")))
        return;
    fake_door::answerJson(2, QJsonObject{ { "ok", true }, { "address", "0xabc" } });

    expectCall(3, QStringLiteral("keystore_module"), QStringLiteral("list_accounts"));

    if (failures == before)
        pass("an import with no label goes straight to the account list");
}

// ── a label that will not stick is not a failed import ───────────────────────
//
// The other half of `labelAccount`, and the DOOR's own failure rather than a
// refusal from the keystore: the two are different reasons for the same outcome
// and the backend reads them out of different fields. Either way the key is in
// the keystore, so the import stays reported as done and the list is still
// re-read — a wallet that hid a new account because its name did not take would
// be the worse of the two wrongs.
void aRefusedLabelDoesNotUnsayTheImport()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.importMnemonic(phraseJson(), QStringLiteral("main"));
    if (!admitTheCustodian(0)
        || !expectCall(2, QStringLiteral("keystore_module"), QStringLiteral("import_mnemonic")))
        return;
    fake_door::answerJson(2, QJsonObject{ { "ok", true }, { "address", "0xabc" } });

    if (!expectCall(3, QStringLiteral("keystore_module"), QStringLiteral("set_label")))
        return;
    fake_door::failCall(3, QStringLiteral("the door never delivered it"));

    check(backend.statusText().contains(QStringLiteral("Account imported")),
          QStringLiteral("a label that did not stick unsaid the import: %1")
              .arg(backend.statusText()));
    check(backend.statusText().contains(QStringLiteral("the door never delivered it")),
          QStringLiteral("the reason the label did not stick never reached the view: %1")
              .arg(backend.statusText()));
    expectCall(4, QStringLiteral("keystore_module"), QStringLiteral("list_accounts"));

    if (failures == before)
        pass("a label that will not stick leaves the import reported as done");
}

// ── a missing phrase is answered here ────────────────────────────────────────
void anEmptyPhraseIsRefusedWithoutAsking()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const QJsonObject answer = parse(backend.importMnemonic(
        QStringLiteral("{\"accountIndex\":0,\"password\":\"pw\"}"), QStringLiteral("main")));
    check(!answer.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("an import with no phrase was accepted"));
    check(fake_door::calls.isEmpty(),
          QStringLiteral("an import with no phrase still asked the keystore: %1 calls")
              .arg(fake_door::calls.size()));
    check(!backend.statusText().contains(QStringLiteral("no mobile build")),
          QStringLiteral("a missing phrase is reported as a missing build: %1")
              .arg(backend.statusText()));

    if (failures == before)
        pass("an import with no phrase is refused here, without a round trip");
}

// ── what really has no mobile build still says so ────────────────────────────
//
// The other four tabs' refusals are TRUE, and they are the reason #147 was
// confusing rather than obviously wrong. A fix that made every refusal vaguer
// would cost more than it gained, so the true ones are asserted too.
void theCoordinatorsSurfaceStillRefusesByName()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const auto refusesNaming = [&backend](const QString& reply, const QString& module) {
        const QJsonObject answer = parse(reply);
        check(!answer.value(QStringLiteral("ok")).toBool(),
              QStringLiteral("a method this variant cannot serve answered ok: %1").arg(reply));
        check(answer.value(QStringLiteral("error")).toString().contains(module),
              QStringLiteral("the refusal does not name %1: %2").arg(module, reply));
    };

    refusesNaming(backend.estimateFee(QStringLiteral("{}")),
                  QStringLiteral("wallet_backend_module"));
    refusesNaming(backend.sendNative(QStringLiteral("{}")),
                  QStringLiteral("wallet_backend_module"));
    refusesNaming(backend.sendStatus(QStringLiteral("handle")),
                  QStringLiteral("wallet_backend_module"));

    backend.refreshHistory(QStringLiteral("0xabc"));
    check(backend.statusText().contains(QStringLiteral("wallet_backend_module")),
          QStringLiteral("the history refusal does not name its module: %1")
              .arg(backend.statusText()));

    check(fake_door::calls.isEmpty(),
          QStringLiteral("a refusal by name still called out: %1 calls")
              .arg(fake_door::calls.size()));

    if (failures == before)
        pass("the coordinator's own surface still refuses by name");
}

// ── the token list comes out of token_list_module ────────────────────────────
//
// #148: this tab answered "Token lists needs token_list_module, which has no
// mobile build" and made NO CALL, because that module published no
// `mobile.<target>.bare` and so could not be in a phone's Bundled set. It can
// now, so the asking is what is checked — and `get_tokens` is deliberately the
// WHOLE of it: an unconfigured token_list already serves its shipped offline
// list, so a tab that only reads needs no `configure` and issues no network I/O
// on a user's first tap.
void theTokenListComesFromItsOwnModule()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.loadTokens(1);
    check(!backend.statusText().contains(QStringLiteral("no mobile build")),
          QStringLiteral("the status line still says token_list has no mobile build: %1")
              .arg(backend.statusText()));

    const std::optional<fake_door::Call> asked =
        expectCall(0, QStringLiteral("token_list_module"), QStringLiteral("get_tokens"));
    if (!asked)
        return;
    check(asked->args.size() == 1 && asked->args.at(0).toInt() == 1,
          QStringLiteral("get_tokens was not asked for the chain the view named: %1")
              .arg(describe(*asked)));
    // Nothing else goes out: reading a list is one call, not a configure first.
    check(fake_door::calls.size() == 1,
          QStringLiteral("loading a token list made %1 call(s), not 1")
              .arg(fake_door::calls.size()));

    const QJsonObject row{ { "chainId", 1 },
                           { "address", "0x1f98…F984" },
                           { "name", "Uniswap" },
                           { "symbol", "UNI" },
                           { "decimals", 18 },
                           { "source", "embedded" } };
    fake_door::answerJson(0, QJsonObject{ { "ok", true }, { "tokens", QJsonArray{ row } } });

    // The view reads `tokens` off this PROP, so the module's rows have to reach
    // it — a reply that was fetched and then dropped is the same blank tab.
    const QJsonArray published = parse(backend.tokensJson()).value(QStringLiteral("tokens")).toArray();
    check(published.size() == 1
              && published.at(0).toObject().value(QStringLiteral("symbol")).toString()
                  == QStringLiteral("UNI"),
          QStringLiteral("the module's rows did not reach the view: %1").arg(backend.tokensJson()));
    // The COUNT, not a bare "1" — which "chain 1" would satisfy on its own and
    // so would a status line that never learned how many rows arrived.
    check(backend.statusText().contains(QStringLiteral("1 token(s)")),
          QStringLiteral("the status line does not say what was loaded: %1")
              .arg(backend.statusText()));

    if (failures == before)
        pass("a token list is one get_tokens on token_list_module, and it reaches the view");
}

// ── a refusal from token_list says what token_list said ──────────────────────
void aTokenListRefusalIsReportedInItsOwnWords()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.loadTokens(11155111);
    if (!expectCall(0, QStringLiteral("token_list_module"), QStringLiteral("get_tokens")))
        return;
    fake_door::answerJson(0, QJsonObject{ { "ok", false },
                                          { "error", "token_list context not ready" } });

    check(backend.statusText().contains(QStringLiteral("token_list context not ready")),
          QStringLiteral("token_list's own reason did not reach the view: %1")
              .arg(backend.statusText()));
    check(!backend.statusText().contains(QStringLiteral("no mobile build")),
          QStringLiteral("a refused read is reported as a missing build: %1")
              .arg(backend.statusText()));
    // An empty list, not the previous chain's rows: a tab that kept showing
    // chain 1's tokens under a failed chain 11155111 would be wrong silently.
    check(parse(backend.tokensJson()).value(QStringLiteral("tokens")).toArray().isEmpty(),
          QStringLiteral("a refused read left rows on the tab: %1").arg(backend.tokensJson()));

    if (failures == before)
        pass("a refused token list reports token_list's reason and empties the tab");
}

// ── a custom token is stored by the module that owns the list ────────────────
void aCustomTokenGoesToTheModuleAndTheListIsReread()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const QString tokenJson = QString::fromUtf8(
        QJsonDocument(QJsonObject{ { "chainId", 137 },
                                   { "address", "0xdead" },
                                   { "name", "Mine" },
                                   { "symbol", "MINE" },
                                   { "decimals", 6 } })
            .toJson(QJsonDocument::Compact));
    check(backend.addCustomToken(tokenJson),
          QStringLiteral("addCustomToken refused instead of taking the ask: %1")
              .arg(backend.statusText()));

    const std::optional<fake_door::Call> added =
        expectCall(0, QStringLiteral("token_list_module"), QStringLiteral("add_custom_token"));
    if (!added)
        return;
    // ONE params document, carrying what the caller wrote — the shape
    // `add_custom_token` deserializes into a Token. Re-rendered on the way out
    // rather than forwarded verbatim, so what is asserted is the FIELDS.
    const QJsonObject sent =
        parse(added->args.isEmpty() ? QString() : added->args.at(0).toString());
    check(sent.value(QStringLiteral("address")).toString() == QStringLiteral("0xdead")
              && sent.value(QStringLiteral("chainId")).toInt() == 137,
          QStringLiteral("the token did not reach the module intact: %1").arg(describe(*added)));
    // A BARE BOOL, which is what `add_custom_token` answers — not the `{ok,…}`
    // envelope the readers use. The two are different on this wire.
    fake_door::answerBool(0, true);

    // ...and the tab is re-read FOR THE CHAIN THE TOKEN WAS ADDED ON, so the
    // row the user just typed is visible without a second tap.
    const std::optional<fake_door::Call> reread =
        expectCall(1, QStringLiteral("token_list_module"), QStringLiteral("get_tokens"));
    if (!reread)
        return;
    check(reread->args.size() == 1 && reread->args.at(0).toInt() == 137,
          QStringLiteral("the re-read was not for the token's chain: %1").arg(describe(*reread)));

    if (failures == before)
        pass("a custom token is stored by token_list_module and the chain is re-read");
}

// ── a token the module would not store is not reported as added ──────────────
//
// `add_custom_token` answers a BARE BOOL, so "no" carries no reason — and the
// `.rep` contract already returned true here, meaning only that the ask was
// taken. If the refusal were dropped the user would be told "Token added" over
// a list that never gained the row, which is the one outcome worse than an
// error. Nothing is re-read either: there is nothing new to see.
void aTokenTheModuleRefusesIsNotReportedAsAdded()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.addCustomToken(QStringLiteral("{\"chainId\":1,\"address\":\"0xdead\"}"));
    if (!expectCall(0, QStringLiteral("token_list_module"), QStringLiteral("add_custom_token")))
        return;
    fake_door::answerBool(0, false);

    check(!backend.statusText().contains(QStringLiteral("Token added")),
          QStringLiteral("a refused token was reported as added: %1").arg(backend.statusText()));
    check(fake_door::calls.size() == 1,
          QStringLiteral("a refused add still re-read the list: %1 calls")
              .arg(fake_door::calls.size()));

    if (failures == before)
        pass("a token token_list would not store is not reported as added");
}

// ── a token document this image can see is wrong is answered here ────────────
void aTokenWithNoAddressIsRefusedWithoutAsking()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    check(!backend.addCustomToken(QStringLiteral("{\"chainId\":1,\"symbol\":\"X\"}")),
          QStringLiteral("a token with no address was accepted"));
    check(fake_door::calls.isEmpty(),
          QStringLiteral("a token with no address still asked the module: %1 calls")
              .arg(fake_door::calls.size()));
    check(!backend.statusText().contains(QStringLiteral("no mobile build")),
          QStringLiteral("a malformed token is reported as a missing build: %1")
              .arg(backend.statusText()));

    if (failures == before)
        pass("a token with no address is refused here, without a round trip");
}


// ── the sync a private send waits on, in windows the view can see ────────────
//
// logos-workspace#235: a private send is ~154 s on a phone and ~92 % of it is
// the accumulator sync, which `railgun_module.sync()` does in ONE call that
// reports nothing and cannot be interrupted. `sync_step` is the same work in
// bounded windows; what is asserted here is that this variant WALKS them —
// asks for the next one only when the last has answered, publishes the
// progress in between, and stops the moment the plan is done.
QJsonObject planReply(int percent, int syncedBlock, bool done)
{
    return QJsonObject{ { "ok", true },
                        { "done", done },
                        { "percent", percent },
                        { "startBlock", 11720000 },
                        { "syncedBlock", syncedBlock },
                        { "targetBlock", 11721000 },
                        { "blocksTotal", 1000 },
                        { "blocksDone", syncedBlock - 11720000 },
                        { "blocksRemaining", 11721000 - syncedBlock },
                        { "windows", 1 },
                        { "elapsedMs", 5000 },
                        { "etaMs", done ? QJsonValue() : QJsonValue(6000) } };
}

QJsonObject privateSync(const WalletUiWebBackend& backend)
{
    return parse(backend.privateSyncJson()).value(QStringLiteral("sync")).toObject();
}

void theSyncIsWalkedInWindowsAndTheViewSeesEachOne()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const QJsonObject taken = parse(backend.startPrivateSync());
    check(taken.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("startPrivateSync did not take the ask: %1")
              .arg(QString::fromUtf8(QJsonDocument(taken).toJson(QJsonDocument::Compact))));

    // ONE WINDOW IS ASKED FOR, and the budget is in the ask: a window that ran
    // for the module's own default (20 s) would repaint the view three times a
    // minute, which is the spinner this issue exists to remove.
    const std::optional<fake_door::Call> first =
        expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_step"));
    if (!first)
        return;
    // A STRING, not an object: `sync_step(params_json: String)` refuses an
    // object by name, which cost a device run to find (logos-workspace#235).
    check(first->args.size() == 1 && first->args.at(0).isString(),
          QStringLiteral("sync_step was not given its params as one JSON string: %1")
              .arg(describe(*first)));
    const QJsonObject params = parse(first->args.isEmpty() ? QString()
                                                           : first->args.at(0).toString());
    check(params.value(QStringLiteral("budgetMs")).toInt(0) > 0
              && params.value(QStringLiteral("budgetMs")).toInt(0) <= 10000,
          QStringLiteral("a window's budget is not sized for a view: %1").arg(describe(*first)));

    fake_door::answerJson(0, planReply(40, 11720400, false));

    const QJsonObject running = privateSync(backend);
    check(running.value(QStringLiteral("state")).toString() == QStringLiteral("running"),
          QStringLiteral("the surface is not running after a window: %1")
              .arg(backend.privateSyncJson()));
    check(running.value(QStringLiteral("percent")).toInt(-1) == 40,
          QStringLiteral("the percentage did not reach the view: %1").arg(backend.privateSyncJson()));
    check(running.value(QStringLiteral("blocksRemaining")).toInt(-1) == 600,
          QStringLiteral("the blocks left did not reach the view: %1").arg(backend.privateSyncJson()));
    check(running.value(QStringLiteral("etaMs")).toInt(-1) == 6000,
          QStringLiteral("the ETA did not reach the view: %1").arg(backend.privateSyncJson()));
    check(running.value(QStringLiteral("leg")).toString() == QStringLiteral("sync"),
          QStringLiteral("the running leg is not named: %1").arg(backend.privateSyncJson()));

    // ...and the NEXT window is asked for only now, from inside the reply.
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("sync_step")))
        return;
    fake_door::answerJson(1, planReply(100, 11721000, true));

    const QJsonObject finished = privateSync(backend);
    check(finished.value(QStringLiteral("state")).toString() == QStringLiteral("done"),
          QStringLiteral("a finished plan is not reported done: %1").arg(backend.privateSyncJson()));
    check(fake_door::calls.size() == 2,
          QStringLiteral("a finished plan was stepped again: %1 calls")
              .arg(fake_door::calls.size()));

    if (failures == before)
        pass("the accumulator sync is walked one window at a time, and each one reaches the view");
}

// ── leaving is: stop asking for windows ──────────────────────────────────────
//
// #235's second clause. 154 s is long enough that a user will try to leave, and
// leaving must not corrupt anything. It cannot, and this asserts the mechanism
// that makes that true rather than the claim: the walk is one window at a time,
// so a cancel is "do not ask for the next one" — and the window already in
// flight is allowed to land, because a sync only reads the chain and every
// window it completes is persisted before it returns.
void cancellingStopsAskingAndSaysWhatItLeft()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.startPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_step")))
        return;
    fake_door::answerJson(0, planReply(40, 11720400, false));
    // The reply chained the next window, which is now in flight.
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("sync_step")))
        return;

    backend.cancelPrivateSync();
    const std::optional<fake_door::Call> cancelled =
        expectCall(2, QStringLiteral("railgun_module"), QStringLiteral("sync_cancel"));
    if (!cancelled)
        return;

    // The in-flight window lands AFTER the user has left. It must not chain a
    // third one — that is the whole of the cancel.
    fake_door::answerJson(1, planReply(70, 11720700, false));
    check(fake_door::calls.size() == 3,
          QStringLiteral("a window was asked for after the cancel: %1 calls")
              .arg(fake_door::calls.size()));

    QJsonObject reply = planReply(70, 11720700, false);
    reply.insert(QStringLiteral("cancelled"), true);
    reply.insert(QStringLiteral("keptToBlock"), 11720700);
    fake_door::answerJson(2, reply);

    const QJsonObject state = privateSync(backend);
    check(state.value(QStringLiteral("state")).toString() == QStringLiteral("cancelled"),
          QStringLiteral("the surface does not report the cancel: %1").arg(backend.privateSyncJson()));
    // THE POST-CANCEL STATE, SHOWN. #235 asks the wallet to decide and document
    // what a cancel does to a shield that is already mined; the answer — nothing
    // — is on the surface the user is looking at, not only in a rustdoc.
    const QString note = state.value(QStringLiteral("note")).toString();
    check(note.contains(QStringLiteral("11720700")),
          QStringLiteral("the block the walk reached is not shown: %1").arg(note));
    check(note.contains(QStringLiteral("shield")) && note.contains(QStringLiteral("untouched")),
          QStringLiteral("the note does not say what a mined shield does: %1").arg(note));

    if (failures == before)
        pass("a cancel stops asking for windows, and the view is told what it left");
}

// ── an uninitialised engine keeps its own words ──────────────────────────────
void aModuleRefusalIsReportedInItsOwnWords()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.refreshPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    fake_door::answerJson(0, QJsonObject{ { "ok", false },
                                          { "error", "railgun_module not initialized (call init first)" } });

    check(privateSync(backend).value(QStringLiteral("error")).toString().contains(
              QStringLiteral("not initialized")),
          QStringLiteral("the module's own reason was dropped: %1").arg(backend.privateSyncJson()));

    if (failures == before)
        pass("an uninitialised engine is reported in railgun_module's own words");
}

// ── one walk at a time ───────────────────────────────────────────────────────
//
// `railgun_module` is `concurrency: single`, so a second chain of windows would
// queue behind the first and spend the radio twice for one plan — and the view
// would see the percentage flip between two readings of it.
void aSecondStartDoesNotDoubleTheWalk()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.startPrivateSync();
    const QJsonObject second = parse(backend.startPrivateSync());
    check(!second.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("a second start was accepted: %1")
              .arg(QString::fromUtf8(QJsonDocument(second).toJson(QJsonDocument::Compact))));
    check(fake_door::calls.size() == 1,
          QStringLiteral("a second start asked for another window: %1 calls")
              .arg(fake_door::calls.size()));

    if (failures == before)
        pass("a second start is refused rather than walking the same plan twice");
}

// ── a sync that stops moving is not looped on ────────────────────────────────
void aStalledSyncStopsAskingAndSaysWhereItGotTo()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.startPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_step")))
        return;
    QJsonObject stalled = planReply(40, 11720400, false);
    stalled.insert(QStringLiteral("stalled"), true);
    fake_door::answerJson(0, stalled);

    check(fake_door::calls.size() == 1,
          QStringLiteral("a stalled sync was stepped again: %1 calls")
              .arg(fake_door::calls.size()));
    const QJsonObject state = privateSync(backend);
    check(state.value(QStringLiteral("state")).toString() == QStringLiteral("idle"),
          QStringLiteral("a stalled sync still reads as running: %1").arg(backend.privateSyncJson()));
    check(state.value(QStringLiteral("note")).toString().contains(QStringLiteral("11720400")),
          QStringLiteral("a stalled sync does not say where it got to: %1")
              .arg(backend.privateSyncJson()));

    // ...and it can be started again: the module plans afresh from where it is.
    check(parse(backend.startPrivateSync()).value(QStringLiteral("ok")).toBool(),
          QStringLiteral("a stalled sync cannot be retried"));

    if (failures == before)
        pass("a sync that stops making progress is reported, not looped on");
}

// ── the first status read races the core's load path ─────────────────────────
//
// MEASURED ON AN iPad Air 13-inch SIMULATOR, which is how this case came to
// exist. The page asked `keystore_module` and `railgun_module` in the same turn
// and BOTH were refused with "token not recognized (re-exchange failed)" — the
// core had not finished registering this module's credential. `refreshAccounts`
// retries, so the account list arrived two seconds later; the private surface
// did not, and it published `unavailable` naming railgun for a module that was
// loaded and answering in the same run. A refusal by the DOOR that early is a
// race, not an answer, and the same retry that carries the account list has to
// carry this.
//
// The retry is bounded and the LAST refusal still lands on the surface: a build
// that really has no railgun_module must not poll for the life of the page.
void theFirstStatusReadRetriesThroughTheAdmissionRace()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.refreshPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    // THE DOOR refused it, not the module: `res.ok` false, which is the shape a
    // tokenless call comes back in.
    fake_door::failCall(0, QStringLiteral(
        "call to 'railgun_module' rejected: token not recognized (re-exchange failed)"));

    check(privateSync(backend).value(QStringLiteral("state")).toString()
              != QStringLiteral("unavailable"),
          QStringLiteral("a refusal during the admission race was published as a verdict: %1")
              .arg(backend.privateSyncJson()));
    settleUntilCall(1, 3000);
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    fake_door::answerJson(1, planReply(100, 11721000, true));
    check(privateSync(backend).value(QStringLiteral("state")).toString() == QStringLiteral("done"),
          QStringLiteral("the retry's answer did not reach the view: %1")
              .arg(backend.privateSyncJson()));

    if (failures == before)
        pass("the first status read retries through the core's admission race");
}

// ── ...and gives up, so a build with no railgun is still named ───────────────
//
// The other half of the retry. `railgun_module` is not declared a dependency of
// this variant (an image carries it only when it was bundled, and declaring it
// would stop the wallet loading on every build that did not), so "absent" is a
// state this surface has to report rather than wait out.
void theRetryIsBoundedAndTheLastRefusalIsPublished()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.refreshPrivateSync();
    // Refuse every attempt, until the backend stops making them. Bounded by a
    // CALL CEILING rather than by a deadline, so a retry budget that grew would
    // be REPORTED here rather than hanging the run.
    const int ceiling = 32;
    int refused = 0;
    while (refused < ceiling && refused < fake_door::calls.size()) {
        fake_door::failCall(refused, QStringLiteral("module not found: railgun_module"));
        ++refused;
        settleUntilCall(refused, 3000);
    }
    check(refused < ceiling,
          QStringLiteral("the retry never gave up: %1 calls").arg(refused));
    check(privateSync(backend).value(QStringLiteral("state")).toString()
              == QStringLiteral("unavailable"),
          QStringLiteral("a build with no railgun never reached a verdict: %1")
              .arg(backend.privateSyncJson()));
    check(privateSync(backend).value(QStringLiteral("error")).toString().contains(
              QStringLiteral("railgun_module")),
          QStringLiteral("the last refusal does not name the module: %1")
              .arg(backend.privateSyncJson()));
    // NAMED ON THE STATUS LINE TOO, and with no percentage: a build that does
    // not carry railgun must not render a convincing 0 % for a walk that can
    // never start.
    check(backend.statusText().contains(QStringLiteral("railgun_module")),
          QStringLiteral("the status line does not name the module: %1").arg(backend.statusText()));
    check(!privateSync(backend).contains(QStringLiteral("percent")),
          QStringLiteral("an unavailable sync still shows a percentage: %1")
              .arg(backend.privateSyncJson()));

    if (failures == before)
        pass("the admission retry gives up, and the last refusal names the module");
}


// ── a private SEND, leg by leg ───────────────────────────────────────────────
//
// logos-workspace#235 clause 1: "which leg is running (wrap / approve / shield /
// sync / prove / broadcast) and that the app has not hung". The sync above is
// the long leg; this is the send it is a leg OF, and what is asserted is that
// every leg is named as it runs, that a leg is only marked done by a reply that
// landed, and that the route is one call at a time so there is always exactly
// one thing to cancel.
QJsonObject sendState(const WalletUiWebBackend& backend)
{
    return parse(backend.privateSendJson()).value(QStringLiteral("send")).toObject();
}

// The state of one named leg in the `legs` array, or "" if the route does not
// carry it. A view renders the whole route, so a drive asserts on the whole
// route rather than on `leg` alone.
QString legState(const WalletUiWebBackend& backend, const QString& name)
{
    const QJsonArray legs = sendState(backend).value(QStringLiteral("legs")).toArray();
    for (const QJsonValue& v : legs) {
        const QJsonObject leg = v.toObject();
        if (leg.value(QStringLiteral("name")).toString() == name)
            return leg.value(QStringLiteral("state")).toString();
    }
    return QString();
}

QString sendJson(const QString& to = QStringLiteral("0zk1qyxj0wxcpfdzq4sxa34vqx7kmzuxka34n4xdcxf9"))
{
    return QString::fromUtf8(
        QJsonDocument(
            QJsonObject{ { "to", to },
                         { "asset", "0xfFf9976782d46CC05630D1f6eBAb18b2324d6B14" },
                         { "amount", "1000000000000000" },
                         { "memo", "" },
                         { "owner", "0x493A0000000000000000000000000000000dDEEa" },
                         { "bundlerUrl", "https://bundler.example/rpc" } })
            .toJson(QJsonDocument::Compact));
}

// `sync_status` for a device that is behind the head, which is the state a
// wallet is in before it has ever walked.
QJsonObject behindReply()
{
    QJsonObject r = planReply(0, 11720000, false);
    r.insert(QStringLiteral("running"), false);
    return r;
}

void aPrivateSendNamesEveryLegItRuns()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    // The wallet already knows the distance — one eth_blockNumber, taken
    // automatically. It has walked nothing.
    backend.refreshPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    fake_door::answerJson(0, behindReply());

    const QJsonObject taken = parse(backend.startPrivateSend(sendJson()));
    check(taken.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("startPrivateSend did not take the ask: %1")
              .arg(QString::fromUtf8(QJsonDocument(taken).toJson(QJsonDocument::Compact))));

    // LEG 1 — sync. THE SEND STARTS THE WALK (#235 clause 4): the distance is
    // read for free and automatically, the walk happens when something needs
    // the tree.
    check(sendState(backend).value(QStringLiteral("leg")).toString() == QStringLiteral("sync"),
          QStringLiteral("a send that is behind did not start on the sync leg: %1")
              .arg(backend.privateSendJson()));
    check(legState(backend, QStringLiteral("sync")) == QStringLiteral("running"),
          QStringLiteral("the sync leg is not running: %1").arg(backend.privateSendJson()));
    check(legState(backend, QStringLiteral("prove")) == QStringLiteral("pending"),
          QStringLiteral("a later leg is not pending: %1").arg(backend.privateSendJson()));
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("sync_step")))
        return;
    fake_door::answerJson(1, planReply(100, 11721000, true));

    // LEG 2 — prove. One `relayed_send`, and its params are one JSON STRING
    // like every other rust-first method on this wire.
    const std::optional<fake_door::Call> prove =
        expectCall(2, QStringLiteral("railgun_module"), QStringLiteral("relayed_send"));
    if (!prove)
        return;
    check(prove->args.size() == 1 && prove->args.at(0).isString(),
          QStringLiteral("relayed_send was not given its params as one JSON string: %1")
              .arg(describe(*prove)));
    const QJsonObject params = parse(prove->args.isEmpty() ? QString()
                                                           : prove->args.at(0).toString());
    check(params.value(QStringLiteral("bundlerUrl")).toString()
              == QStringLiteral("https://bundler.example/rpc"),
          QStringLiteral("the bundler the caller named did not reach the module: %1")
              .arg(describe(*prove)));
    check(legState(backend, QStringLiteral("sync")) == QStringLiteral("done"),
          QStringLiteral("the sync leg is not done once the walk finished: %1")
              .arg(backend.privateSendJson()));
    check(sendState(backend).value(QStringLiteral("leg")).toString() == QStringLiteral("prove"),
          QStringLiteral("the running leg is not the proof: %1").arg(backend.privateSendJson()));
    fake_door::answerJson(2, QJsonObject{ { "ok", true },
                                          { "pending", true },
                                          { "requestId", "handle-1" } });

    // LEG 3 — approve. The request is lodged; a human answers it in the Signer
    // app, and the wallet polls for that answer.
    const std::optional<fake_door::Call> poll =
        expectCall(3, QStringLiteral("railgun_module"), QStringLiteral("relayed_send_status"));
    if (!poll)
        return;
    check(poll->args.size() == 1 && poll->args.at(0).toString() == QStringLiteral("handle-1"),
          QStringLiteral("the status poll did not carry the request id: %1").arg(describe(*poll)));
    check(sendState(backend).value(QStringLiteral("leg")).toString() == QStringLiteral("approve"),
          QStringLiteral("the running leg is not the approval: %1").arg(backend.privateSendJson()));
    check(legState(backend, QStringLiteral("prove")) == QStringLiteral("done"),
          QStringLiteral("the proof leg is not done: %1").arg(backend.privateSendJson()));
    fake_door::answerJson(3, QJsonObject{ { "ok", true }, { "state", "awaiting_approval" } });

    // ...and it is polled AGAIN, on a timer, because a human takes wall time.
    settleUntilCall(4, 5000);
    if (!expectCall(4, QStringLiteral("railgun_module"), QStringLiteral("relayed_send_status")))
        return;
    fake_door::answerJson(4, QJsonObject{ { "ok", true },
                                          { "state", "done" },
                                          { "userOpHash", "0xfeed" } });

    // LEG 4 — broadcast. The poll that answered `done` is the one that
    // submitted the operation to the bundler, so the leg is reported by its
    // result and never as "running": the wallet cannot see it start.
    check(sendState(backend).value(QStringLiteral("state")).toString() == QStringLiteral("done"),
          QStringLiteral("a submitted send is not done: %1").arg(backend.privateSendJson()));
    check(legState(backend, QStringLiteral("broadcast")) == QStringLiteral("done"),
          QStringLiteral("the broadcast leg is not done: %1").arg(backend.privateSendJson()));
    check(sendState(backend).value(QStringLiteral("userOpHash")).toString()
              == QStringLiteral("0xfeed"),
          QStringLiteral("the operation hash did not reach the view: %1")
              .arg(backend.privateSendJson()));

    if (failures == before)
        pass("a private send names every leg it runs, and each one ends on a reply that landed");
}

// ── a tree already at the head is not walked again ───────────────────────────
//
// The other half of #235 clause 4. `sync_status` is one RPC and is taken
// automatically, so the wallet already knows it is level — and a send that
// walked anyway would spend minutes of radio for nothing. The leg is SKIPPED,
// which is a different word from `done` on purpose: a view that showed "synced"
// for a walk it never made would be claiming work it did not do.
void aSendOnASyncedTreeSkipsTheWalk()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.refreshPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    fake_door::answerJson(0, planReply(100, 11721000, true));

    backend.startPrivateSend(sendJson());

    // Straight to the proof: no `sync_step` was ever asked for.
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("relayed_send")))
        return;
    check(legState(backend, QStringLiteral("sync")) == QStringLiteral("skipped"),
          QStringLiteral("a level tree was not reported as skipped: %1")
              .arg(backend.privateSendJson()));
    check(sendState(backend).value(QStringLiteral("leg")).toString() == QStringLiteral("prove"),
          QStringLiteral("a level tree did not go straight to the proof: %1")
              .arg(backend.privateSendJson()));

    if (failures == before)
        pass("a send on a tree that is already at the head skips the walk and says so");
}

// ── leaving, and what leaving costs at each leg ──────────────────────────────
//
// #235 clause 2 for the whole send rather than for the sync alone. The route is
// one call at a time, so there is always exactly one thing in flight and
// "cancel" means something different — and states something different — at each
// leg. Here: during the approval, where a request is sitting in a human's queue
// and NOTHING has been signed or broadcast.
void cancellingDuringTheApprovalWithdrawsTheRequest()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.refreshPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    fake_door::answerJson(0, planReply(100, 11721000, true));
    backend.startPrivateSend(sendJson());
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("relayed_send")))
        return;
    fake_door::answerJson(1, QJsonObject{ { "ok", true },
                                          { "pending", true },
                                          { "requestId", "handle-1" } });
    if (!expectCall(2, QStringLiteral("railgun_module"), QStringLiteral("relayed_send_status")))
        return;
    fake_door::answerJson(2, QJsonObject{ { "ok", true }, { "state", "awaiting_approval" } });

    const QJsonObject taken = parse(backend.cancelPrivateSend());
    check(taken.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("a cancel during the approval was refused: %1")
              .arg(QString::fromUtf8(QJsonDocument(taken).toJson(QJsonDocument::Compact))));

    // THE REQUEST IS WITHDRAWN, by id: a request nobody withdraws sits in the
    // approver's queue until the keystore sweeps it, and the user who left is
    // the one who would be asked about it.
    const std::optional<fake_door::Call> withdrawn =
        expectCall(3, QStringLiteral("railgun_module"), QStringLiteral("relayed_send_cancel"));
    if (!withdrawn)
        return;
    check(withdrawn->args.size() == 1
              && withdrawn->args.at(0).toString() == QStringLiteral("handle-1"),
          QStringLiteral("the withdrawal did not name the request: %1").arg(describe(*withdrawn)));
    fake_door::answerJson(3, QJsonObject{ { "ok", true } });

    check(sendState(backend).value(QStringLiteral("state")).toString()
              == QStringLiteral("cancelled"),
          QStringLiteral("the send does not report the cancel: %1").arg(backend.privateSendJson()));
    const QString note = sendState(backend).value(QStringLiteral("note")).toString();
    check(note.contains(QStringLiteral("nothing"), Qt::CaseInsensitive)
              && note.contains(QStringLiteral("broadcast")),
          QStringLiteral("the note does not say that nothing was broadcast: %1").arg(note));

    // ...AND THE POLL STOPS. A timer that kept asking about a request that has
    // been withdrawn would answer "unknown request" for the life of the page.
    const int after = fake_door::calls.size();
    settleUntilCall(after, 3000);
    check(fake_door::calls.size() == after,
          QStringLiteral("the approval was still polled after the cancel: %1 calls")
              .arg(fake_door::calls.size()));

    if (failures == before)
        pass("a cancel during the approval withdraws the request, and nothing was broadcast");
}

// ── a broadcast send is the chain's ──────────────────────────────────────────
//
// The one leg where "cancel" has no meaning, and the surface says so rather
// than offering a button that quietly does nothing.
void aBroadcastSendCannotBeRecalled()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.refreshPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    fake_door::answerJson(0, planReply(100, 11721000, true));
    backend.startPrivateSend(sendJson());
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("relayed_send")))
        return;
    fake_door::answerJson(1, QJsonObject{ { "ok", true },
                                          { "pending", true },
                                          { "requestId", "handle-1" } });
    if (!expectCall(2, QStringLiteral("railgun_module"), QStringLiteral("relayed_send_status")))
        return;
    fake_door::answerJson(2, QJsonObject{ { "ok", true },
                                          { "state", "done" },
                                          { "userOpHash", "0xfeed" } });

    const int after = fake_door::calls.size();
    const QJsonObject refused = parse(backend.cancelPrivateSend());
    check(!refused.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("a broadcast send accepted a cancel: %1")
              .arg(QString::fromUtf8(QJsonDocument(refused).toJson(QJsonDocument::Compact))));
    check(refused.value(QStringLiteral("error")).toString().contains(QStringLiteral("broadcast")),
          QStringLiteral("the refusal does not say why: %1")
              .arg(refused.value(QStringLiteral("error")).toString()));
    check(fake_door::calls.size() == after,
          QStringLiteral("a broadcast send still asked the module to cancel: %1 calls")
              .arg(fake_door::calls.size()));
    check(sendState(backend).value(QStringLiteral("state")).toString() == QStringLiteral("done"),
          QStringLiteral("a refused cancel unsaid the send: %1").arg(backend.privateSendJson()));

    if (failures == before)
        pass("a send that has been broadcast refuses a cancel and stays done");
}

// ── a human said no ──────────────────────────────────────────────────────────
void aDeclinedApprovalEndsTheSendInItsOwnWords()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.refreshPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    fake_door::answerJson(0, planReply(100, 11721000, true));
    backend.startPrivateSend(sendJson());
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("relayed_send")))
        return;
    fake_door::answerJson(1, QJsonObject{ { "ok", true },
                                          { "pending", true },
                                          { "requestId", "handle-1" } });
    if (!expectCall(2, QStringLiteral("railgun_module"), QStringLiteral("relayed_send_status")))
        return;
    fake_door::answerJson(2, QJsonObject{ { "ok", true },
                                          { "state", "declined" },
                                          { "reason", "rejected" } });

    check(sendState(backend).value(QStringLiteral("state")).toString() == QStringLiteral("failed"),
          QStringLiteral("a declined send is not reported failed: %1").arg(backend.privateSendJson()));
    check(legState(backend, QStringLiteral("approve")) == QStringLiteral("failed"),
          QStringLiteral("the approval leg is not the one that failed: %1")
              .arg(backend.privateSendJson()));
    check(sendState(backend).value(QStringLiteral("error")).toString().contains(
              QStringLiteral("rejected")),
          QStringLiteral("the signer's own reason was dropped: %1").arg(backend.privateSendJson()));

    // ...and nothing is polled after a decision.
    const int after = fake_door::calls.size();
    settleUntilCall(after, 3000);
    check(fake_door::calls.size() == after,
          QStringLiteral("a declined send was still polled: %1 calls").arg(fake_door::calls.size()));

    if (failures == before)
        pass("a declined approval ends the send, in the signer's own words");
}

// ── a send this wallet cannot even ask for ───────────────────────────────────
//
// The same rule the custom-token field follows: what the wallet can see is
// wrong, it says so HERE, without spending a round trip and without a refusal
// that names a module as if the module were the problem.
void aSendWithNoBundlerIsRefusedWithoutAsking()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    QJsonObject params = parse(sendJson());
    params.remove(QStringLiteral("bundlerUrl"));
    const QJsonObject refused = parse(backend.startPrivateSend(
        QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));

    check(!refused.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("a send with no bundler was accepted: %1")
              .arg(QString::fromUtf8(QJsonDocument(refused).toJson(QJsonDocument::Compact))));
    check(refused.value(QStringLiteral("error")).toString().contains(QStringLiteral("bundler")),
          QStringLiteral("the refusal does not name the missing field: %1")
              .arg(refused.value(QStringLiteral("error")).toString()));
    check(fake_door::calls.isEmpty(),
          QStringLiteral("a malformed send still asked a module: %1 calls")
              .arg(fake_door::calls.size()));
    check(!backend.statusText().contains(QStringLiteral("no mobile build")),
          QStringLiteral("a malformed send is reported as a missing build: %1")
              .arg(backend.statusText()));

    if (failures == before)
        pass("a send with a field missing is refused here, without a round trip");
}

// ── one send at a time, and it joins the walk already running ────────────────
//
// `railgun_module` is `concurrency: single`. A second send would queue behind
// the first; a send started while the user is already walking the tree must
// take that walk as its sync leg rather than asking for a second chain of
// windows against the same plan.
void aSendJoinsAWalkAlreadyRunning()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.startPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_step")))
        return;

    backend.startPrivateSend(sendJson());
    check(sendState(backend).value(QStringLiteral("leg")).toString() == QStringLiteral("sync"),
          QStringLiteral("a send started during a walk is not on the sync leg: %1")
              .arg(backend.privateSendJson()));
    check(fake_door::calls.size() == 1,
          QStringLiteral("a send started a second chain of windows: %1 calls")
              .arg(fake_door::calls.size()));

    // The walk that was already running finishes, and the send it was adopted
    // by carries on into the proof.
    fake_door::answerJson(0, planReply(100, 11721000, true));
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("relayed_send")))
        return;
    check(legState(backend, QStringLiteral("sync")) == QStringLiteral("done"),
          QStringLiteral("the adopted walk did not complete the sync leg: %1")
              .arg(backend.privateSendJson()));

    // ...and a SECOND send is refused rather than queued.
    const QJsonObject second = parse(backend.startPrivateSend(sendJson()));
    check(!second.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("a second private send was accepted: %1")
              .arg(QString::fromUtf8(QJsonDocument(second).toJson(QJsonDocument::Compact))));

    if (failures == before)
        pass("a send takes the walk already running as its sync leg, and there is only ever one send");
}

// ── cancelling during the walk keeps what the walk did ───────────────────────
void cancellingDuringTheSyncLegKeepsTheBlocks()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    backend.refreshPrivateSync();
    if (!expectCall(0, QStringLiteral("railgun_module"), QStringLiteral("sync_status")))
        return;
    fake_door::answerJson(0, behindReply());
    backend.startPrivateSend(sendJson());
    if (!expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("sync_step")))
        return;

    backend.cancelPrivateSend();
    const std::optional<fake_door::Call> cancelled =
        expectCall(2, QStringLiteral("railgun_module"), QStringLiteral("sync_cancel"));
    if (!cancelled)
        return;
    QJsonObject reply = planReply(40, 11720400, false);
    reply.insert(QStringLiteral("cancelled"), true);
    reply.insert(QStringLiteral("keptToBlock"), 11720400);
    fake_door::answerJson(2, reply);

    // The window that was in flight lands after the user has left, and must not
    // carry the send on into a proof nobody asked for any more.
    fake_door::answerJson(1, planReply(100, 11721000, true));
    check(fake_door::calls.size() == 3,
          QStringLiteral("a cancelled send still went on to prove: %1 calls")
              .arg(fake_door::calls.size()));

    check(sendState(backend).value(QStringLiteral("state")).toString()
              == QStringLiteral("cancelled"),
          QStringLiteral("the send does not report the cancel: %1").arg(backend.privateSendJson()));
    const QString note = sendState(backend).value(QStringLiteral("note")).toString();
    check(note.contains(QStringLiteral("11720400")),
          QStringLiteral("the note does not name the block the walk kept: %1").arg(note));

    if (failures == before)
        pass("a cancel during the walk keeps every window it finished, and no proof is asked for");
}


// ── the other direction: public funds going INTO the pool ────────────────────
//
// #235 clause 1 names six legs — "wrap / approve / shield / sync / prove /
// broadcast" — and the cases above cover the last three, the send of a balance
// that is already shielded. These cover the first three, which are how a
// balance gets there, and the two that make them possible: `plan` (the calldata
// and the numbers) and `sign` (ONE approval request, answered once by a human).
//
// WHAT IS BEING ASSERTED, as everywhere in this file: the ASKING. Which module,
// which method, with what arguments, in what order — and in particular that the
// allowance is asked for BEFORE the shield with a consecutive nonce, because
// that ordering is the whole reason all three can go out without a receipt
// between them.

// The RailgunSmartWallet, as `prepare_shield` names it in the `to` of the
// transaction it answers with. That address is where the allowance goes, and
// asserting it comes from the reply rather than from a constant in the wallet
// is the point of the case below.
const char* kRailgunWallet = "0x1c7d4b196cb0c7b01d743fbc6116a902379c7238";
const char* kWeth = "0xfFf9976782d46CC05630D1f6eBAb18b2324d6B14";
const char* kOwner = "0x493A0000000000000000000000000000000dDEEa";

QString shieldJson(bool wrap = true, const QString& amount = QStringLiteral("1000000000000000"))
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{ { "chainId", 11155111 },
                                   { "owner", kOwner },
                                   { "asset", kWeth },
                                   { "amount", amount },
                                   { "wrap", wrap } })
            .toJson(QJsonDocument::Compact));
}

QJsonObject shieldState(const WalletUiWebBackend& backend)
{
    return parse(backend.privateShieldJson()).value(QStringLiteral("shield")).toObject();
}

QString shieldLeg(const WalletUiWebBackend& backend, const QString& name)
{
    for (const QJsonValue& v : shieldState(backend).value(QStringLiteral("legs")).toArray()) {
        const QJsonObject leg = v.toObject();
        if (leg.value(QStringLiteral("name")).toString() == name)
            return leg.value(QStringLiteral("state")).toString();
    }
    return QString();
}

QJsonObject shieldTxsReply()
{
    return QJsonObject{ { "ok", true },
                        { "txs", QJsonArray{ QJsonObject{ { "to", kRailgunWallet },
                                                          { "data", "0xdeadbeef" },
                                                          { "value", "0x0" } } } } };
}

// Drive the route as far as the approval request, answering every read on the
// way. Returns the index of the `request_approval` call, or -1 if the sequence
// broke — every case below starts here, and a case that re-spelled it would be
// asserting the plan four times over.
int planAShield(WalletUiWebBackend& backend, bool wrap = true)
{
    backend.startPrivateShield(shieldJson(wrap));

    // The chain is configured before anything is asked OF it — the same rule
    // every eth_rpc read in this file follows.
    if (!expectCall(0, QStringLiteral("eth_rpc_module"), QStringLiteral("set_chain_config")))
        return -1;
    fake_door::answerBool(0, true);

    const std::optional<fake_door::Call> prep =
        expectCall(1, QStringLiteral("railgun_module"), QStringLiteral("prepare_shield"));
    if (!prep)
        return -1;
    check(prep->args.size() == 1 && prep->args.at(0).isString(),
          QStringLiteral("prepare_shield was not given its params as one JSON string: %1")
              .arg(describe(*prep)));
    fake_door::answerJson(1, shieldTxsReply());

    const std::optional<fake_door::Call> nonce =
        expectCall(2, QStringLiteral("eth_rpc_module"), QStringLiteral("get_transaction_count"));
    if (!nonce)
        return -1;
    check(nonce->args.size() == 2 && nonce->args.at(1).toString() == QLatin1String(kOwner),
          QStringLiteral("the nonce was not read for the account that signs: %1")
              .arg(describe(*nonce)));
    fake_door::answerJson(2, QJsonObject{ { "ok", true }, { "result", "0x7" } });

    if (!expectCall(3, QStringLiteral("eth_rpc_module"), QStringLiteral("gas_price")))
        return -1;
    fake_door::answerJson(3, QJsonObject{ { "ok", true }, { "result", "0x3b9aca00" } });
    return 4;
}

// Answer the approval request and the poll that follows it, and hand back the
// signatures. `legs` is how many transactions the bundle carries.
bool approveAShield(int requestCall, int legs)
{
    if (!expectCall(requestCall, QStringLiteral("keystore_module"),
                    QStringLiteral("request_approval")))
        return false;
    fake_door::answerJson(requestCall, QJsonObject{ { "ok", true },
                                                    { "handle", "ap-1" },
                                                    { "receipt", "rc-1" } });

    if (!expectCall(requestCall + 1, QStringLiteral("keystore_module"),
                    QStringLiteral("approval_status")))
        return false;
    fake_door::answerJson(requestCall + 1,
                          QJsonObject{ { "ok", true }, { "state", "settled" },
                                       { "reason", "approved" } });

    if (!expectCall(requestCall + 2, QStringLiteral("keystore_module"),
                    QStringLiteral("fetch_result")))
        return false;
    QJsonArray signed_;
    for (int i = 0; i < legs; ++i)
        signed_.append(QStringLiteral("0xraw%1").arg(i));
    fake_door::answerJson(requestCall + 2, QJsonObject{ { "ok", true }, { "signed", signed_ } });
    return true;
}

// ── the route is named before it is walked ───────────────────────────────────
//
// #235's "the app has not hung" starts before anything is running: a page that
// names the five legs it would take, from its first paint, has already told a
// user what a shield consists of and that nothing has been started.
void theShieldRouteIsPublishedFromTheFirstPaint()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    check(shieldState(backend).value(QStringLiteral("state")).toString() == QStringLiteral("idle"),
          QStringLiteral("a fresh page does not publish an idle shield: %1")
              .arg(backend.privateShieldJson()));
    const QJsonArray legs = shieldState(backend).value(QStringLiteral("legs")).toArray();
    QStringList named;
    for (const QJsonValue& v : legs)
        named << v.toObject().value(QStringLiteral("name")).toString();
    check(named == QStringList({ "plan", "sign", "wrap", "approve", "shield" }),
          QStringLiteral("the route is not named in the order it is walked: %1")
              .arg(named.join(QStringLiteral(", "))));
    check(shieldState(backend).value(QStringLiteral("cancellable")).toBool() == false,
          QStringLiteral("a shield nobody started offers a cancel: %1")
              .arg(backend.privateShieldJson()));
    check(fake_door::calls.isEmpty(),
          QStringLiteral("naming the route spent %1 call(s)").arg(fake_door::calls.size()));

    if (failures == before)
        pass("the shield's route is named from the first paint, before anything is walked");
}

void aShieldNamesEveryLegItPutsOnChain()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const int request = planAShield(backend);
    if (request < 0)
        return;

    // LEG 1 — plan. Done by the time the human is asked: the calldata, the
    // nonce and the fee are all in hand, and none of them cost the user a
    // decision.
    check(shieldLeg(backend, QStringLiteral("plan")) == QStringLiteral("done"),
          QStringLiteral("the plan leg is not done once the reads have landed: %1")
              .arg(backend.privateShieldJson()));
    check(shieldState(backend).value(QStringLiteral("leg")).toString() == QStringLiteral("sign"),
          QStringLiteral("the running leg is not the signature: %1")
              .arg(backend.privateShieldJson()));

    // LEG 2 — sign. ONE request, carrying the whole route: the human sees the
    // wrap, the allowance and the shield together and answers once.
    const std::optional<fake_door::Call> ask =
        expectCall(request, QStringLiteral("keystore_module"), QStringLiteral("request_approval"));
    if (!ask)
        return;
    const QJsonObject intent =
        parse(ask->args.isEmpty() ? QString() : ask->args.at(0).toString());
    check(intent.value(QStringLiteral("address")).toString() == QLatin1String(kOwner),
          QStringLiteral("the intent is not addressed to the account that signs: %1")
              .arg(describe(*ask)));
    const QJsonArray legs = intent.value(QStringLiteral("legs")).toArray();
    check(legs.size() == 3,
          QStringLiteral("a wrap+approve+shield route was not one bundle of three: %1")
              .arg(describe(*ask)));
    if (legs.size() != 3)
        return;

    // THE ORDER IS THE CONTRACT, and so are the nonces: the allowance must be
    // executed before the shield, and consecutive nonces are what makes the
    // chain do that without this wallet waiting for a receipt in between.
    const QJsonObject wrapTx = legs.at(0).toObject().value(QStringLiteral("tx")).toObject();
    const QJsonObject allowTx = legs.at(1).toObject().value(QStringLiteral("tx")).toObject();
    const QJsonObject shieldTx = legs.at(2).toObject().value(QStringLiteral("tx")).toObject();
    check(wrapTx.value(QStringLiteral("nonce")).toString() == QStringLiteral("0x7")
              && allowTx.value(QStringLiteral("nonce")).toString() == QStringLiteral("0x8")
              && shieldTx.value(QStringLiteral("nonce")).toString() == QStringLiteral("0x9"),
          QStringLiteral("the bundle's nonces are not consecutive from the account's: %1")
              .arg(describe(*ask)));

    // The wrap is WETH9's payable `deposit()` ON THE ASSET ITSELF — that is
    // what turns native ETH into the ERC-20 RAILGUN can take.
    check(wrapTx.value(QStringLiteral("to")).toString().toLower()
              == QString(QLatin1String(kWeth)).toLower(),
          QStringLiteral("the wrap is not a call on the asset: %1").arg(describe(*ask)));
    check(wrapTx.value(QStringLiteral("data")).toString() == QStringLiteral("0xd0e30db0"),
          QStringLiteral("the wrap is not deposit(): %1").arg(describe(*ask)));
    check(wrapTx.value(QStringLiteral("value")).toString() == QStringLiteral("1000000000000000"),
          QStringLiteral("the wrap does not pay the amount being shielded: %1")
              .arg(describe(*ask)));

    // The allowance is `approve(spender, amount)` on the asset, and the SPENDER
    // is the address `prepare_shield` answered with — read off the reply, never
    // a constant in this wallet.
    const QString allowData = allowTx.value(QStringLiteral("data")).toString();
    check(allowData.startsWith(QStringLiteral("0x095ea7b3")),
          QStringLiteral("the allowance leg is not approve(): %1").arg(allowData));
    check(allowData.contains(QString(QLatin1String(kRailgunWallet)).mid(2)),
          QStringLiteral("the allowance does not name the wallet prepare_shield answered "
                         "with as its spender: %1")
              .arg(allowData));
    check(allowData.endsWith(QStringLiteral("00038d7ea4c68000")),
          QStringLiteral("the allowance is not for the amount being shielded: %1").arg(allowData));

    // ...and the shield is the transaction the module built, passed through.
    check(shieldTx.value(QStringLiteral("to")).toString() == QLatin1String(kRailgunWallet)
              && shieldTx.value(QStringLiteral("data")).toString() == QStringLiteral("0xdeadbeef"),
          QStringLiteral("the shield leg is not the module's own transaction: %1")
              .arg(describe(*ask)));

    if (!approveAShield(request, 3))
        return;
    check(shieldLeg(backend, QStringLiteral("sign")) == QStringLiteral("done"),
          QStringLiteral("the sign leg is not done once the signatures landed: %1")
              .arg(backend.privateShieldJson()));

    // LEGS 3-5 — the chain. Every raw transaction goes out back to back: the
    // nonces already order them, so waiting for a receipt between them would
    // only make the user wait twelve seconds longer per leg for nothing.
    for (int i = 0; i < 3; ++i) {
        const std::optional<fake_door::Call> sent =
            expectCall(request + 3 + i, QStringLiteral("eth_rpc_module"),
                       QStringLiteral("send_raw_transaction"));
        if (!sent)
            return;
        check(sent->args.size() == 2
                  && sent->args.at(1).toString() == QStringLiteral("0xraw%1").arg(i),
              QStringLiteral("transaction %1 was not the one the keystore signed: %2")
                  .arg(i)
                  .arg(describe(*sent)));
        fake_door::answerJson(request + 3 + i,
                              QJsonObject{ { "ok", true },
                                           { "hash", QStringLiteral("0xh%1").arg(i) } });
    }

    // The signatures have been spent, so the keystore is told to wipe its copy.
    if (!expectCall(request + 6, QStringLiteral("keystore_module"), QStringLiteral("ack_result")))
        return;
    fake_door::answerBool(request + 6, true);

    // A LEG IS `running` WHEN ITS TRANSACTION IS IN A MEMPOOL AND `done` WHEN
    // IT IS MINED, which is the only honest reading of a wait the chain owns.
    check(shieldLeg(backend, QStringLiteral("wrap")) == QStringLiteral("running"),
          QStringLiteral("a broadcast leg is not running: %1").arg(backend.privateShieldJson()));

    for (int i = 0; i < 3; ++i) {
        const std::optional<fake_door::Call> receipt =
            expectCall(request + 7 + i, QStringLiteral("eth_rpc_module"),
                       QStringLiteral("get_transaction_receipt"));
        if (!receipt)
            return;
        check(receipt->args.size() == 2
                  && receipt->args.at(1).toString() == QStringLiteral("0xh%1").arg(i),
              QStringLiteral("receipt %1 was not followed for the hash it was sent under: %2")
                  .arg(i)
                  .arg(describe(*receipt)));
        fake_door::answerJson(request + 7 + i,
                              QJsonObject{ { "ok", true },
                                           { "result", QJsonObject{ { "status", "0x1" },
                                                                    { "blockNumber", "0x64" } } } });
    }

    check(shieldLeg(backend, QStringLiteral("wrap")) == QStringLiteral("done")
              && shieldLeg(backend, QStringLiteral("approve")) == QStringLiteral("done")
              && shieldLeg(backend, QStringLiteral("shield")) == QStringLiteral("done"),
          QStringLiteral("a mined route did not end with every leg done: %1")
              .arg(backend.privateShieldJson()));
    check(shieldState(backend).value(QStringLiteral("state")).toString() == QStringLiteral("done"),
          QStringLiteral("a mined route is not done: %1").arg(backend.privateShieldJson()));
    check(shieldState(backend).value(QStringLiteral("cancellable")).toBool() == false,
          QStringLiteral("a finished shield still offers a cancel: %1")
              .arg(backend.privateShieldJson()));

    if (failures == before)
        pass("a shield names every leg it puts on chain, signs them as one bundle with "
             "consecutive nonces, and follows each to its receipt");
}


// ── a leg that was not needed is not a leg that ran ──────────────────────────
//
// The same distinction the send's route makes between `skipped` and `done`, on
// the one leg of a shield that is optional. A caller shielding an ERC-20 they
// already hold does not wrap anything, and a route that reported `done` for
// that would be claiming a transaction the wallet never sent.
void aShieldWithoutAWrapSaysSkippedAndNotDone()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const int request = planAShield(backend, /*wrap=*/false);
    if (request < 0)
        return;

    check(shieldLeg(backend, QStringLiteral("wrap")) == QStringLiteral("skipped"),
          QStringLiteral("a route that wraps nothing did not say skipped: %1")
              .arg(backend.privateShieldJson()));

    const std::optional<fake_door::Call> ask =
        expectCall(request, QStringLiteral("keystore_module"), QStringLiteral("request_approval"));
    if (!ask)
        return;
    const QJsonArray legs = parse(ask->args.at(0).toString())
                                .value(QStringLiteral("legs"))
                                .toArray();
    check(legs.size() == 2,
          QStringLiteral("a route with no wrap still asked a human to sign three things: %1")
              .arg(describe(*ask)));
    if (legs.size() != 2)
        return;
    check(legs.at(0).toObject().value(QStringLiteral("tx")).toObject()
              .value(QStringLiteral("nonce")).toString() == QStringLiteral("0x7"),
          QStringLiteral("the allowance did not take the account's own next nonce: %1")
              .arg(describe(*ask)));

    if (failures == before)
        pass("a shield that wraps nothing skips the leg, and signs two transactions "
             "rather than three");
}

// ── leaving, and what leaving costs on this route ────────────────────────────
//
// #235 clause 2 for the direction that spends money. While a human is deciding,
// nothing has been signed and nothing has been broadcast: the request comes out
// of the Signer's queue and the note says so where the user is looking.
void cancellingWhileTheHumanDecidesTakesTheRequestBack()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const int request = planAShield(backend);
    if (request < 0)
        return;
    if (!expectCall(request, QStringLiteral("keystore_module"),
                    QStringLiteral("request_approval")))
        return;
    fake_door::answerJson(request, QJsonObject{ { "ok", true },
                                                { "handle", "ap-1" },
                                                { "receipt", "rc-1" } });
    if (!expectCall(request + 1, QStringLiteral("keystore_module"),
                    QStringLiteral("approval_status")))
        return;
    fake_door::answerJson(request + 1, QJsonObject{ { "ok", true }, { "state", "offered" } });

    check(shieldState(backend).value(QStringLiteral("cancellable")).toBool(),
          QStringLiteral("a shield waiting on a human is not offered a cancel: %1")
              .arg(backend.privateShieldJson()));

    const QJsonObject taken = parse(backend.cancelPrivateShield());
    check(taken.value(QStringLiteral("ok")).toBool(),
          QStringLiteral("cancelPrivateShield did not take the ask: %1")
              .arg(backend.privateShieldJson()));

    // The request is WITHDRAWN, by handle and receipt — not merely forgotten
    // on this side, which would leave it sitting in a human's queue.
    const std::optional<fake_door::Call> withdraw =
        expectCall(request + 2, QStringLiteral("keystore_module"),
                   QStringLiteral("cancel_approval"));
    if (!withdraw)
        return;
    check(withdraw->args.size() == 2 && withdraw->args.at(0).toString() == QStringLiteral("ap-1")
              && withdraw->args.at(1).toString() == QStringLiteral("rc-1"),
          QStringLiteral("the withdrawal did not name the request it takes back: %1")
              .arg(describe(*withdraw)));
    fake_door::answerBool(request + 2, true);

    check(shieldState(backend).value(QStringLiteral("state")).toString()
              == QStringLiteral("cancelled"),
          QStringLiteral("a withdrawn shield is not cancelled: %1")
              .arg(backend.privateShieldJson()));
    check(shieldLeg(backend, QStringLiteral("sign")) == QStringLiteral("cancelled"),
          QStringLiteral("the sign leg is not cancelled: %1").arg(backend.privateShieldJson()));
    check(shieldState(backend).value(QStringLiteral("note")).toString()
              .contains(QStringLiteral("Nothing was spent")),
          QStringLiteral("the note does not say what was not done: %1")
              .arg(backend.privateShieldJson()));
    // NOT ONE TRANSACTION WENT OUT.
    for (const fake_door::Call& c : fake_door::calls)
        check(c.method != QStringLiteral("send_raw_transaction"),
              QStringLiteral("a cancelled shield still broadcast something: %1").arg(describe(c)));

    if (failures == before)
        pass("a cancel while a human is deciding takes the request back, and nothing "
             "reached the chain");
}

// ── ...and the point where there is nothing to take back ─────────────────────
//
// The other half of clause 2, and a HARDER line than the send's: three
// transactions signed with consecutive nonces are handed over back to back, so
// the moment the first leaves, the route is the chain's.
void aBroadcastShieldCannotBeRecalled()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const int request = planAShield(backend);
    if (request < 0 || !approveAShield(request, 3))
        return;

    // The first transaction is in flight and has not even been answered yet.
    if (!expectCall(request + 3, QStringLiteral("eth_rpc_module"),
                    QStringLiteral("send_raw_transaction")))
        return;
    check(shieldState(backend).value(QStringLiteral("cancellable")).toBool() == false,
          QStringLiteral("a shield already handed to the chain still offers a cancel: %1")
              .arg(backend.privateShieldJson()));

    const QJsonObject refused = parse(backend.cancelPrivateShield());
    check(refused.value(QStringLiteral("ok")).toBool() == false,
          QStringLiteral("cancelling a broadcast shield was accepted: %1")
              .arg(backend.privateShieldJson()));
    check(refused.value(QStringLiteral("error")).toString()
              .contains(QStringLiteral("ordered by nonce")),
          QStringLiteral("the refusal does not say why it cannot be recalled: %1")
              .arg(refused.value(QStringLiteral("error")).toString()));
    check(fake_door::calls.size() == request + 4,
          QStringLiteral("the refused cancel still spent a call: %1 call(s) in all")
              .arg(fake_door::calls.size()));

    if (failures == before)
        pass("a shield whose transactions have been handed over refuses a cancel, and "
             "says they are nonce-ordered and cannot be recalled");
}

// ── the human said no ────────────────────────────────────────────────────────
void aDeclinedShieldEndsInTheSignersOwnWords()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const int request = planAShield(backend);
    if (request < 0)
        return;
    if (!expectCall(request, QStringLiteral("keystore_module"),
                    QStringLiteral("request_approval")))
        return;
    fake_door::answerJson(request, QJsonObject{ { "ok", true },
                                                { "handle", "ap-1" },
                                                { "receipt", "rc-1" } });
    if (!expectCall(request + 1, QStringLiteral("keystore_module"),
                    QStringLiteral("approval_status")))
        return;
    fake_door::answerJson(request + 1, QJsonObject{ { "ok", true },
                                                    { "state", "settled" },
                                                    { "reason", "rejected" } });

    check(shieldState(backend).value(QStringLiteral("state")).toString()
              == QStringLiteral("failed"),
          QStringLiteral("a declined shield did not end: %1").arg(backend.privateShieldJson()));
    check(shieldState(backend).value(QStringLiteral("error")).toString()
              .contains(QStringLiteral("rejected")),
          QStringLiteral("the decline is not reported in the signer's own word: %1")
              .arg(backend.privateShieldJson()));
    check(fake_door::calls.size() == request + 2,
          QStringLiteral("a declined shield asked for the signatures anyway: %1 call(s)")
              .arg(fake_door::calls.size()));

    if (failures == before)
        pass("a declined shield ends there, in the signer's own word, and never asks "
             "for signatures");
}

// ── a transaction that was mined and reverted ────────────────────────────────
//
// The failure this route cannot prevent and must not hide: gas is spent, the
// state did not change, and every leg after it will not execute either because
// the nonces are consecutive.
void aMinedRevertIsReportedAsOne()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const int request = planAShield(backend);
    if (request < 0 || !approveAShield(request, 3))
        return;
    for (int i = 0; i < 3; ++i) {
        if (!expectCall(request + 3 + i, QStringLiteral("eth_rpc_module"),
                        QStringLiteral("send_raw_transaction")))
            return;
        fake_door::answerJson(request + 3 + i,
                              QJsonObject{ { "ok", true },
                                           { "hash", QStringLiteral("0xh%1").arg(i) } });
    }
    if (!expectCall(request + 6, QStringLiteral("keystore_module"), QStringLiteral("ack_result")))
        return;
    fake_door::answerBool(request + 6, true);

    // The wrap is mined; the allowance reverts.
    if (!expectCall(request + 7, QStringLiteral("eth_rpc_module"),
                    QStringLiteral("get_transaction_receipt")))
        return;
    fake_door::answerJson(request + 7,
                          QJsonObject{ { "ok", true },
                                       { "result", QJsonObject{ { "status", "0x1" } } } });
    if (!expectCall(request + 8, QStringLiteral("eth_rpc_module"),
                    QStringLiteral("get_transaction_receipt")))
        return;
    fake_door::answerJson(request + 8,
                          QJsonObject{ { "ok", true },
                                       { "result", QJsonObject{ { "status", "0x0" } } } });

    check(shieldLeg(backend, QStringLiteral("wrap")) == QStringLiteral("done"),
          QStringLiteral("the leg that WAS mined is not done: %1")
              .arg(backend.privateShieldJson()));
    check(shieldLeg(backend, QStringLiteral("approve")) == QStringLiteral("failed"),
          QStringLiteral("the reverted leg is not failed: %1").arg(backend.privateShieldJson()));
    check(shieldState(backend).value(QStringLiteral("error")).toString()
              .contains(QStringLiteral("reverted")),
          QStringLiteral("a mined revert is not named as one: %1")
              .arg(backend.privateShieldJson()));
    check(fake_door::calls.size() == request + 9,
          QStringLiteral("the route kept going after a revert: %1 call(s)")
              .arg(fake_door::calls.size()));

    if (failures == before)
        pass("a transaction that was mined and reverted is named as one, and the route "
             "stops there");
}

// ── what the wallet can see is wrong, it says here ───────────────────────────
//
// The same rule `missingSendField` follows. An amount that is not a decimal
// whole number cannot be encoded into an ERC-20 argument at all, so refusing it
// here costs no round trip and names nothing that is not at fault.
void aShieldWithAnUnencodableAmountIsRefusedWithoutAsking()
{
    const int before = failures;
    fake_door::reset();
    WalletUiWebBackend backend;

    const QJsonObject refused = parse(backend.startPrivateShield(shieldJson(true, "0.001")));
    check(refused.value(QStringLiteral("ok")).toBool() == false,
          QStringLiteral("a fractional amount was accepted: %1")
              .arg(backend.privateShieldJson()));
    check(refused.value(QStringLiteral("error")).toString()
              .contains(QStringLiteral("base units")),
          QStringLiteral("the refusal does not say what an amount has to be: %1")
              .arg(refused.value(QStringLiteral("error")).toString()));
    check(fake_door::calls.isEmpty(),
          QStringLiteral("a refused shield still spent %1 call(s)")
              .arg(fake_door::calls.size()));

    // ...AND THE PANEL THE USER IS LOOKING AT SAYS SO. Measured on an iPad Air
    // 13-inch simulator: a device with no account in its keystore pressed
    // `Shield`, the form had no `owner`, and `privateShieldJson` was never
    // published — so the refusal existed only on the status line and the route
    // panel was blank (logos-workspace#235).
    check(shieldState(backend).value(QStringLiteral("error")).toString() == refused.value(QStringLiteral("error")).toString(),
          QStringLiteral("the refusal did not reach the shield's own surface: %1")
              .arg(backend.privateShieldJson()));
    check(shieldLeg(backend, QStringLiteral("plan")) == QStringLiteral("pending"),
          QStringLiteral("a refused shield did not lay its route out fresh: %1")
              .arg(backend.privateShieldJson()));

    if (failures == before)
        pass("an amount this wallet cannot encode is refused here, without a round trip, "
             "and the refusal reaches the panel the user is looking at");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    importGoesToTheKeystore();
    aKeystoreRefusalIsReportedInItsOwnWords();
    withoutALabelNothingIsLabelled();
    aRefusedLabelDoesNotUnsayTheImport();
    anEmptyPhraseIsRefusedWithoutAsking();
    theCoordinatorsSurfaceStillRefusesByName();
    theTokenListComesFromItsOwnModule();
    aTokenListRefusalIsReportedInItsOwnWords();
    aCustomTokenGoesToTheModuleAndTheListIsReread();
    aTokenTheModuleRefusesIsNotReportedAsAdded();
    aTokenWithNoAddressIsRefusedWithoutAsking();
    theSyncIsWalkedInWindowsAndTheViewSeesEachOne();
    cancellingStopsAskingAndSaysWhatItLeft();
    aModuleRefusalIsReportedInItsOwnWords();
    aSecondStartDoesNotDoubleTheWalk();
    aStalledSyncStopsAskingAndSaysWhereItGotTo();
    theFirstStatusReadRetriesThroughTheAdmissionRace();
    theRetryIsBoundedAndTheLastRefusalIsPublished();
    aPrivateSendNamesEveryLegItRuns();
    aSendOnASyncedTreeSkipsTheWalk();
    cancellingDuringTheApprovalWithdrawsTheRequest();
    aBroadcastSendCannotBeRecalled();
    aDeclinedApprovalEndsTheSendInItsOwnWords();
    aSendWithNoBundlerIsRefusedWithoutAsking();
    aSendJoinsAWalkAlreadyRunning();
    cancellingDuringTheSyncLegKeepsTheBlocks();
    theShieldRouteIsPublishedFromTheFirstPaint();
    aShieldNamesEveryLegItPutsOnChain();
    aShieldWithoutAWrapSaysSkippedAndNotDone();
    cancellingWhileTheHumanDecidesTakesTheRequestBack();
    aBroadcastShieldCannotBeRecalled();
    aDeclinedShieldEndsInTheSignersOwnWords();
    aMinedRevertIsReportedAsOne();
    aShieldWithAnUnencodableAmountIsRefusedWithoutAsking();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
