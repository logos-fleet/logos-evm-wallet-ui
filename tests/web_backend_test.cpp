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
    check(backend.statusText().contains(QStringLiteral("1")),
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
    // ONE params document, exactly as the caller wrote it — the shape
    // `add_custom_token` deserializes into a Token.
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

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
