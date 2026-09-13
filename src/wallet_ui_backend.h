#pragma once

#include <QObject>
#include <QString>

#include "rep_wallet_ui_source.h"
#include "logos_ui_plugin_context.h"

// The hand-written UI backend (universal authoring model). We write only this
// class and the wallet_ui.rep contract; the *Plugin / *Interface glue
// (Q_PLUGIN_METADATA, initLogos wiring, QtRO registration) is generated around it.
//
// Derives:
//   - WalletUiSimpleSource — generated from wallet_ui.rep; we implement its SLOTs
//     and feed its PROPs (setStatusText, setBalancesJson, ...), which auto-sync to
//     every QML replica.
//   - LogosUiPluginContext — gives onContextReady() plus modules(), the type-safe
//     callers and event subscriptions for wallet_backend_module.
class WalletUiBackend : public WalletUiSimpleSource,
                        public LogosUiPluginContext
{
public:
    // Config / privacy
    bool setProxyConfig(QString proxyJson) override;
    bool setChains(QString chainsJson) override;
    QString testEndpoint(int chainId) override;

    // Accounts
    QString createAccount(QString passphrase, QString label) override;
    QString importMnemonic(QString phraseJson, QString label) override;
    void refreshAccounts() override;

    // Balances + tokens
    void refreshBalances(QString address) override;
    void loadTokens(int chainId) override;
    bool addCustomToken(QString tokenJson) override;

    // Market (Uniswap prices for held tokens)
    void refreshMarket(QString address) override;

    // Send
    QString estimateFee(QString sendJson) override;
    QString sendNative(QString sendJson) override;
    QString sendErc20(QString sendJson) override;
    QString sendStatus(QString requestId) override;

    // History
    void refreshHistory(QString address) override;

private:
    // Shared by sendNative/sendErc20: record the request id and tell the user
    // where the decision happens.
    QString trackSend(QString reply);

protected:
    // Called once after the framework populates modules()/context.
    void onContextReady() override;
};
