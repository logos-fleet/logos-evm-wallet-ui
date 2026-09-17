#include "wallet_ui_backend.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// Generated umbrella: modules() → typed callers + typed event accessors for the
// modules in metadata.json#dependencies (here: wallet_backend_module for
// everything the coordinator owns, keystore_module for account creation).
#include "logos_sdk.h"

namespace {

QString jsonText(const QJsonObject& obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// Every rust-first module on this wire answers a JSON TEXT, not a structure, so
// a reply is parsed before it is read: `{ "ok": true, … }` on success and
// `{ "ok": false, "error": … }` on a refusal. Named as in the web variant, which
// reads the same envelope.
bool replyOk(const QString& replyJson)
{
    return QJsonDocument::fromJson(replyJson.toUtf8())
        .object()
        .value(QStringLiteral("ok"))
        .toBool();
}

// WHY A WALLET NAMES ITSELF CUSTODIAN. Creating an account is Tier D in the
// keystore: it belongs to the CUSTODIAN, whose built-in default is
// `evm_keystore_ui`. That module does not exist in this workspace, so with the
// defaults in force the New-account dialog would refuse for ever.
//
// ADDED TO THE SET, NOT PUT IN PLACE OF IT. `configure` is TOTAL — a role this
// document does not name is held by NOBODY — so naming only this module would
// revoke `evm_keystore_ui`'s custody and `evm_signer_ui`'s approval in the same
// call. A role is a set precisely so a second holder can be added, and that is
// all this adds. The `web` variant sends the same document; the two are
// separate images with no shared translation unit, so they must stay in step by
// hand.
QString custodianRoles()
{
    QJsonObject roles;
    roles.insert(QStringLiteral("approvers"), QStringLiteral("evm_signer_ui"));
    roles.insert(QStringLiteral("custodians"),
                 QJsonArray{ QStringLiteral("evm_keystore_ui"), QStringLiteral("wallet_ui") });
    return jsonText(roles);
}

} // namespace

void WalletUiBackend::onContextReady()
{
    // Initial state from the backend.
    setChainsJson(modules().wallet_backend_module.get_chains());
    refreshAccounts();

    // Push backend events into PROPs so the QML view updates live.
    modules().wallet_backend_module.onBalances_updated([this](QString address) {
        setBalancesJson(modules().wallet_backend_module.get_balances(address));
        // Balances changed → refresh the Market view: kick off the per-chain price
        // fan-out; the priced result lands via onMarket_updated.
        modules().wallet_backend_module.refresh_market(address);
    });
    modules().wallet_backend_module.onMarket_updated([this](QString address) {
        setMarketJson(modules().wallet_backend_module.get_market(address));
    });
    modules().wallet_backend_module.onTx_status_changed([this](QString) {
        if (!selectedAccount().isEmpty()) {
            setHistoryJson(modules().wallet_backend_module.get_history(selectedAccount()));
        }
    });

    // The Private tab has something true to show from the first paint: this
    // build carries no railgun_module, and says so rather than rendering an
    // empty progress bar.
    refreshPrivateSync();

    setStatusText(QStringLiteral("Ready"));
}

// ── Config / privacy ─────────────────────────────────────────────────────────

bool WalletUiBackend::setProxyConfig(QString proxyJson)
{
    bool ok = modules().wallet_backend_module.set_proxy_config(proxyJson);
    setProxyStatus(ok ? QStringLiteral("Proxy applied") : QStringLiteral("Proxy failed"));
    setStatusText(ok ? QStringLiteral("Proxy applied") : QStringLiteral("Proxy failed"));
    return ok;
}

bool WalletUiBackend::setChains(QString chainsJson)
{
    bool ok = modules().wallet_backend_module.set_chains(chainsJson);
    if (ok) {
        setChainsJson(modules().wallet_backend_module.get_chains());
    }
    setStatusText(ok ? QStringLiteral("Chains updated") : QStringLiteral("Set chains failed"));
    return ok;
}

QString WalletUiBackend::testEndpoint(int chainId)
{
    return modules().wallet_backend_module.test_endpoint(chainId);
}

// ── Accounts ─────────────────────────────────────────────────────────────────

QString WalletUiBackend::createAccount(QString passphrase, QString label)
{
    // Accepted and dropped — see `createAccount` in wallet_ui.rep.
    Q_UNUSED(label)

    // STRAIGHT TO THE KEYSTORE, not through the coordinator. `create_account` left
    // `wallet_backend_module`'s contract when account mutation became Tier D: a
    // wallet backend requests signatures and reads which accounts exist, and a
    // method that existed only to return "not authorized" is worse than no method.
    // So the one surface that still creates an account is this UI, and it asks the
    // keystore itself — `keystore_module` is already one of this module's declared
    // dependencies, so this is one hop rather than three.
    //
    // TAKE THE ROLE, THEN MUTATE: the mutation is refused until the role is in
    // force, so a refusal here is reported instead of asking for the account.
    const QString configured = modules().keystore_module.configure(custodianRoles());
    if (!replyOk(configured)) {
        setStatusText(QStringLiteral("keystore_module refused configure"));
        return configured;
    }

    // THE ACKNOWLEDGEMENT IS THE METHOD'S POINT, not a formality: an unrelated
    // account is a key no recovery phrase covers, and the keystore refuses to mint
    // one unless the caller has said so. The New-account dialog IS that choice.
    QJsonObject params;
    params.insert(QStringLiteral("password"), passphrase);
    params.insert(QStringLiteral("acknowledgeUnrecoverable"), true);
    const QString r = modules().keystore_module.create_unrelated_account(jsonText(params));
    refreshAccounts();
    setStatusText(replyOk(r) ? QStringLiteral("Account created")
                             : QStringLiteral("Account creation refused"));
    return r;
}

QString WalletUiBackend::importMnemonic(QString phraseJson, QString label)
{
    QString r = modules().wallet_backend_module.import_mnemonic(phraseJson, label);
    refreshAccounts();
    setStatusText(QStringLiteral("Account imported"));
    return r;
}

void WalletUiBackend::refreshAccounts()
{
    setAccountsJson(modules().wallet_backend_module.list_accounts());
}

// ── Balances + tokens ────────────────────────────────────────────────────────

void WalletUiBackend::refreshBalances(QString address)
{
    setStatusText(QStringLiteral("Refreshing balances…"));
    // Kicks off the multi-chain fetch (emits balances_updated when done) and
    // returns the last cached aggregate immediately.
    modules().wallet_backend_module.refresh_balances(address);
    setBalancesJson(modules().wallet_backend_module.get_balances(address));
}

void WalletUiBackend::loadTokens(int chainId)
{
    setTokensJson(modules().wallet_backend_module.get_tokens(chainId));
}

bool WalletUiBackend::addCustomToken(QString tokenJson)
{
    bool ok = modules().wallet_backend_module.add_custom_token(tokenJson);
    setStatusText(ok ? QStringLiteral("Token added") : QStringLiteral("Add token failed"));
    return ok;
}

// ── Market ───────────────────────────────────────────────────────────────────

void WalletUiBackend::refreshMarket(QString address)
{
    setStatusText(QStringLiteral("Loading market…"));
    // Kick off the per-chain price fan-out (uniswap is concurrency:"multi", so the
    // chains price in parallel); the priced result lands via onMarket_updated.
    modules().wallet_backend_module.refresh_market(address);
    setStatusText(QStringLiteral("Market updated"));
}

// ── Send ─────────────────────────────────────────────────────────────────────

QString WalletUiBackend::estimateFee(QString sendJson)
{
    return modules().wallet_backend_module.estimate_fee(sendJson);
}

// A send no longer produces a transaction hash here. The wallet asks; a human
// approves in the signer UI; only then is anything signed or broadcast. These
// return a request id, and `sendStatus` carries it the rest of the way.
QString WalletUiBackend::sendNative(QString sendJson)
{
    return trackSend(modules().wallet_backend_module.send_native(sendJson));
}

QString WalletUiBackend::sendErc20(QString sendJson)
{
    return trackSend(modules().wallet_backend_module.send_erc20(sendJson));
}

QString WalletUiBackend::trackSend(QString reply)
{
    const QJsonObject o = QJsonDocument::fromJson(reply.toUtf8()).object();
    if (!o.value(QStringLiteral("ok")).toBool()) {
        setStatusText(QStringLiteral("Send failed"));
        return reply;
    }
    setPendingRequestId(o.value(QStringLiteral("requestId")).toString());
    setStatusText(QStringLiteral("Waiting for approval — open the Signer app"));
    return reply;
}

QString WalletUiBackend::sendStatus(QString requestId)
{
    const QString reply = modules().wallet_backend_module.send_status(requestId);
    const QJsonObject o = QJsonDocument::fromJson(reply.toUtf8()).object();
    const QString state = o.value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("done")) {
        setPendingRequestId(QString());
        setStatusText(QStringLiteral("Sent"));
        refreshHistory(selectedAccount());
    } else if (state == QStringLiteral("declined")) {
        setPendingRequestId(QString());
        setStatusText(QStringLiteral("Declined in the Signer app"));
    }
    return reply;
}

// ── History ──────────────────────────────────────────────────────────────────

void WalletUiBackend::refreshHistory(QString address)
{
    setHistoryJson(modules().wallet_backend_module.get_history(address));
}

// ── the private sync: not on this side of the wallet ─────────────────────────
//
// THE PRIVATE SEND LIVES ON A PHONE. `railgun_module` is a Bundled member of a
// mobile image and is reached by the wallet's `web` variant, which is the half
// that runs there (logos-basecamp ships it as LOGOS_SHELL_WEB_MODULES=wallet_ui).
// This desktop plugin talks to `wallet_backend_module`, and the coordinator does
// not name railgun in its dependencies — checked again this cycle — so there is
// no private send behind this build and nothing here to report progress for.
//
// REFUSED BY NAME rather than answered with a plausible 0 %, which is the rule
// the `web` variant follows in the other direction for the coordinator's own
// surface: a user is told the variant cannot do it and a developer is told which
// module would have served it. The SHAPE is the contract's, so the same QML
// renders both halves — `state: "unavailable"` is a state the Private tab knows.
//
// What it would take to lift this: `railgun_module` in this module's
// `dependencies` and in its flake, which puts the whole RAILGUN engine (a Rust
// crate with the Groth16 circuits) into every desktop wallet build. That is a
// trade worth making when the desktop wallet has a private send to offer, and
// not before.
namespace {

const QString kRailgunAbsent =
    QStringLiteral("Private sends need railgun_module, which this desktop build does not "
                   "carry — the wallet's `web` variant on a phone is the half that has it.");

QString privateSyncUnavailable()
{
    QJsonObject sync;
    sync.insert(QStringLiteral("leg"), QStringLiteral("sync"));
    sync.insert(QStringLiteral("state"), QStringLiteral("unavailable"));
    sync.insert(QStringLiteral("error"), kRailgunAbsent);
    return jsonText(QJsonObject{ { QStringLiteral("sync"), sync } });
}

// THE SEND'S SURFACE, IN THE SAME SHAPE AND WITH THE SAME VERDICT. The route is
// still spelled out — a reader of this build should be able to see what a
// private send would consist of — and every leg carries `unavailable` rather
// than `pending`, because `pending` would promise a leg that is coming.
QString privateSendUnavailable()
{
    QJsonArray legs;
    for (const QString& leg : { QStringLiteral("sync"), QStringLiteral("prove"),
                                QStringLiteral("approve"), QStringLiteral("broadcast") })
        legs.append(QJsonObject{ { QStringLiteral("name"), leg },
                                 { QStringLiteral("state"), QStringLiteral("unavailable") } });
    QJsonObject send;
    send.insert(QStringLiteral("state"), QStringLiteral("unavailable"));
    send.insert(QStringLiteral("leg"), QString());
    send.insert(QStringLiteral("legs"), legs);
    send.insert(QStringLiteral("cancellable"), false);
    send.insert(QStringLiteral("error"), kRailgunAbsent);
    return jsonText(QJsonObject{ { QStringLiteral("send"), send } });
}

// AND THE ROUTE INTO THE POOL, refused in the same shape for the same reason.
// A shield ends in public transactions this account would sign — the signing
// path is `keystore_module`, which this desktop build DOES reach — but the
// calldata comes out of `railgun_module`, and without it there is nothing to
// sign. So the refusal names the module that is missing, not the one that is
// present.
QString privateShieldUnavailable()
{
    QJsonArray legs;
    for (const QString& leg : { QStringLiteral("plan"), QStringLiteral("sign"),
                                QStringLiteral("wrap"), QStringLiteral("approve"),
                                QStringLiteral("shield") })
        legs.append(QJsonObject{ { QStringLiteral("name"), leg },
                                 { QStringLiteral("state"), QStringLiteral("unavailable") } });
    QJsonObject shield;
    shield.insert(QStringLiteral("state"), QStringLiteral("unavailable"));
    shield.insert(QStringLiteral("leg"), QString());
    shield.insert(QStringLiteral("legs"), legs);
    shield.insert(QStringLiteral("cancellable"), false);
    shield.insert(QStringLiteral("error"), kRailgunAbsent);
    return jsonText(QJsonObject{ { QStringLiteral("shield"), shield } });
}

} // namespace

void WalletUiBackend::refreshPrivateSync()
{
    setPrivateSyncJson(privateSyncUnavailable());
    setPrivateSendJson(privateSendUnavailable());
    setPrivateShieldJson(privateShieldUnavailable());
}

// Both controls on the Private tab answer the same way here, because both are
// asking for a module this build does not carry: the surface says `unavailable`
// and the status line says which module would have served it.
QString WalletUiBackend::refuseWithoutRailgun()
{
    setPrivateSyncJson(privateSyncUnavailable());
    setPrivateSendJson(privateSendUnavailable());
    setPrivateShieldJson(privateShieldUnavailable());
    setStatusText(kRailgunAbsent);
    return jsonText(QJsonObject{ { QStringLiteral("ok"), false },
                                 { QStringLiteral("error"), kRailgunAbsent } });
}

QString WalletUiBackend::startPrivateSync()
{
    return refuseWithoutRailgun();
}

QString WalletUiBackend::cancelPrivateSync()
{
    // Nothing is running, so there is nothing to stop — and saying "cancelled"
    // for a walk that never started would be the one answer worse than a refusal.
    return refuseWithoutRailgun();
}

QString WalletUiBackend::startPrivateSend(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuseWithoutRailgun();
}

QString WalletUiBackend::cancelPrivateSend()
{
    return refuseWithoutRailgun();
}

QString WalletUiBackend::startPrivateShield(QString shieldJson)
{
    Q_UNUSED(shieldJson)
    return refuseWithoutRailgun();
}

QString WalletUiBackend::cancelPrivateShield()
{
    return refuseWithoutRailgun();
}
