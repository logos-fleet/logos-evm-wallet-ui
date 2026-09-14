#include "wallet_ui_backend.h"

#include <QJsonDocument>
#include <QJsonObject>

// Generated umbrella: modules() → typed callers + typed event accessors for the
// modules in metadata.json#dependencies (here: wallet_backend_module for
// everything the coordinator owns, keystore_module for account creation).
#include "logos_sdk.h"

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
    Q_UNUSED(label)
    // STRAIGHT TO THE KEYSTORE, not through the coordinator. `create_account` left
    // `wallet_backend_module`'s contract when account mutation became Tier D: a
    // wallet backend requests signatures and reads which accounts exist, and a
    // method that existed only to return "not authorized" is worse than no method.
    // So the one surface that still creates an account is this UI, and it asks the
    // keystore itself — `keystore_module` is already one of this module's declared
    // dependencies, so this is one hop rather than three.
    //
    // Tier D belongs to the CUSTODIAN, whose built-in default is `evm_keystore_ui`.
    // That module does not exist in this workspace, so with the defaults in force
    // the New-account dialog would refuse for ever. This adds this module to the
    // custodian SET — `configure` is TOTAL, so naming only this one would revoke
    // `evm_keystore_ui`'s custody and `evm_signer_ui`'s approval in the same call,
    // and a role is a set precisely so a second holder can be added.
    const QString roles =
        QStringLiteral("{\"approvers\":\"evm_signer_ui\","
                       "\"custodians\":[\"evm_keystore_ui\",\"wallet_ui\"]}");
    const QString configured = modules().keystore_module.configure(roles);
    if (!QJsonDocument::fromJson(configured.toUtf8()).object()
             .value(QStringLiteral("ok")).toBool()) {
        setStatusText(QStringLiteral("keystore_module refused configure"));
        return configured;
    }
    // The acknowledgement is the method's point, not a formality: an unrelated
    // account is a key no recovery phrase covers, and the keystore refuses to mint
    // one unless the caller has said so. The New-account dialog IS that choice.
    QJsonObject params;
    params.insert(QStringLiteral("password"), passphrase);
    params.insert(QStringLiteral("acknowledgeUnrecoverable"), true);
    const QString r = modules().keystore_module.create_unrelated_account(
        QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact)));
    refreshAccounts();
    setStatusText(QJsonDocument::fromJson(r.toUtf8()).object()
                          .value(QStringLiteral("ok")).toBool()
                      ? QStringLiteral("Account created")
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
