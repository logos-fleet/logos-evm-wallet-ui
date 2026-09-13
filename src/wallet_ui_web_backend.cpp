#include "wallet_ui_web_backend.h"

#include <QJsonDocument>
#include <QJsonValue>

// The only way out of a wasm image. See wallet_ui_web_backend.h for why this
// backend exists at all, and logos_web_module_call.h for what the door is.
#include "logos_web_module_call.h"

namespace {

// The module a balance comes from on a phone: a Bundled Bare module, in the
// app image, reached by name through the container.
const QString kEthRpc = QStringLiteral("eth_rpc_module");
// The module an account comes from: itself a `web` variant on a Store shell,
// which the container publishes by name exactly as it does a native one.
const QString kKeystore = QStringLiteral("keystore_module");

// How often the startup wait asks whether the page has a channel yet, and how
// long it waits before saying it has not. A minute: a cold phone launch mounts
// a webview, fetches a document and instantiates a ~26 MB runtime beside this
// image before the container's bridge resolves.
constexpr int kStartupPollMs = 100;
constexpr int kStartupGiveUpMs = 60000;

// `{ "ok": true, "result": … }` / `{ "ok": false, "error": … }` — eth_rpc's
// envelope, and the keystore's. Read here rather than in each caller so a
// refusal is never mistaken for a value.
QJsonValue resultOf(const QJsonValue& reply)
{
    const QJsonObject obj = reply.toObject();
    if (!obj.value(QStringLiteral("ok")).toBool())
        return QJsonValue();
    return obj.value(QStringLiteral("result"));
}

QString errorOf(const QJsonValue& reply)
{
    return reply.toObject().value(QStringLiteral("error")).toString();
}

QString jsonText(const QJsonObject& obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

} // namespace

WalletUiWebBackend::WalletUiWebBackend(QObject* parent)
    : WalletUiSimpleSource(parent)
{
    seedDefaultChains();
    publishChains();
    setAccountsJson(QStringLiteral("{\"accounts\":[]}"));
    setBalancesJson(QStringLiteral("{\"balances\":{\"chains\":[]}}"));
    setStatusText(QStringLiteral("Ready — balances through eth_rpc_module"));

    // AUTOMATIC, AND ONLY AT THE EDGES. The view already picks the first
    // account as soon as one exists (WalletView.qml's `onCountChanged`), and it
    // writes it back through the READWRITE `selectedAccount`. Fetching that
    // account's balances here is what makes the two halves one flow: open the
    // app, see what you hold. A refresh is still one tap, for an account the
    // user picks or a chain list they change.
    connect(this, &WalletUiWebBackend::selectedAccountChanged, this,
            [this](const QString& address) {
                if (!address.isEmpty())
                    refreshBalances(address);
            });

    startWhenReachable();
}

// THE DOOR IS NOT OPEN AT CONSTRUCTION. This image is up before the page has
// bound the container's bridge — the host's own PageChannel says so — so the
// first call has to be made when the channel appears rather than now. A poll
// rather than a signal because the seam that would carry one is JavaScript's,
// on the other side of embind, and a 100 ms tick costs nothing against a page
// that takes seconds to come up.
//
// It gives up. A `web` variant loaded with no host (a page opened by hand, a
// container with no core) would otherwise poll for the life of the process,
// and a module that says what is wrong is worth more than one that keeps
// trying silently.
void WalletUiWebBackend::startWhenReachable()
{
    m_startup.setInterval(kStartupPollMs);
    connect(&m_startup, &QTimer::timeout, this, [this]() {
        if (logos::web::canCallModules()) {
            m_startup.stop();
            refreshAccounts();
            return;
        }
        if (++m_startupTicks * kStartupPollMs > kStartupGiveUpMs) {
            m_startup.stop();
            setStatusText(QStringLiteral(
                "No host channel: this page is not bound to a Logos container"));
        }
    });
    m_startup.start();
}

// THE CHAIN LIST IS THIS VARIANT'S OWN. On the desktop it comes from
// `wallet_backend_module`, which owns the wallet's configuration; there is no
// such module on a phone, and eth_rpc stores an ENDPOINT per chain and nothing
// a view could render (no name, no native symbol). So the variant seeds a list
// and pushes each entry into eth_rpc as configuration — the two halves of what
// the coordinator used to hold, split where the modules actually are.
void WalletUiWebBackend::seedDefaultChains()
{
    const auto chain = [](int id, const char* name, const char* rpc, const char* sym) {
        QJsonObject c;
        c.insert(QStringLiteral("chainId"), id);
        c.insert(QStringLiteral("name"), QLatin1String(name));
        c.insert(QStringLiteral("rpcUrl"), QLatin1String(rpc));
        c.insert(QStringLiteral("nativeSymbol"), QLatin1String(sym));
        return c;
    };
    m_chains = QJsonArray{
        chain(1, "Ethereum", "https://ethereum-rpc.publicnode.com", "ETH"),
        chain(11155111, "Sepolia", "https://ethereum-sepolia-rpc.publicnode.com", "ETH"),
    };
}

void WalletUiWebBackend::publishChains()
{
    QJsonObject root;
    root.insert(QStringLiteral("chains"), m_chains);
    setChainsJson(jsonText(root));
}

QString WalletUiWebBackend::refuse(const QString& what, const QString& module)
{
    const QString message =
        QStringLiteral("%1 needs %2, which has no mobile build — the `web` variant "
                       "does accounts and balances").arg(what, module);
    setStatusText(message);
    QJsonObject out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), message);
    return jsonText(out);
}

// ── config ───────────────────────────────────────────────────────────────────

void WalletUiWebBackend::ensureChainConfig(int chainId, const QString& endpoint)
{
    if (m_chainConfigured.value(chainId, false) || endpoint.isEmpty())
        return;
    m_chainConfigured.insert(chainId, true);

    QJsonObject cfg;
    cfg.insert(QStringLiteral("endpoint"), endpoint);
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("set_chain_config"),
        QJsonArray{ chainId, jsonText(cfg) },
        [this, chainId](const logos::web::ModuleCallResult& res) {
            // A REFUSAL IS NOT CACHED. set_chain_config answers a bare bool, so
            // a `false` — or a call that never reached the module — has to
            // clear the mark or every later balance on this chain would be
            // asked of an endpoint eth_rpc was never told about.
            if (!res.ok || !res.value.toBool()) {
                m_chainConfigured.insert(chainId, false);
                setStatusText(QStringLiteral("eth_rpc_module refused chain %1: %2")
                                  .arg(chainId)
                                  .arg(res.ok ? QStringLiteral("bad config")
                                              : res.error));
            }
        });
}

bool WalletUiWebBackend::setProxyConfig(QString proxyJson)
{
    Q_UNUSED(proxyJson)
    setProxyStatus(QStringLiteral("Proxy config is wallet_backend_module's"));
    setStatusText(QStringLiteral("Proxy config needs wallet_backend_module"));
    return false;
}

bool WalletUiWebBackend::setChains(QString chainsJson)
{
    const QJsonObject root = QJsonDocument::fromJson(chainsJson.toUtf8()).object();
    const QJsonValue chains = root.value(QStringLiteral("chains"));
    const QJsonArray list = chains.isArray() ? chains.toArray()
                                             : QJsonDocument::fromJson(
                                                   chainsJson.toUtf8()).array();
    if (list.isEmpty()) {
        setStatusText(QStringLiteral("Set chains failed: no chains in the list"));
        return false;
    }
    m_chains = list;
    m_chainConfigured.clear();
    publishChains();
    for (const QJsonValue& c : m_chains) {
        const QJsonObject o = c.toObject();
        ensureChainConfig(o.value(QStringLiteral("chainId")).toInt(),
                          o.value(QStringLiteral("rpcUrl")).toString());
    }
    setStatusText(QStringLiteral("Chains updated"));
    return true;
}

// ACCEPTED, NOT ANSWERED. Every `.rep` SLOT with a return value is synchronous
// on this side of the replica and the door is not, so what a caller gets back
// here is that the ask was taken — the answer lands in a PROP. The desktop
// backend can return the real thing; this one structurally cannot, and saying
// so is better than blocking an event loop that would never be pumped.
QString WalletUiWebBackend::testEndpoint(int chainId)
{
    const QJsonObject chain = [&]() -> QJsonObject {
        for (const QJsonValue& c : m_chains)
            if (c.toObject().value(QStringLiteral("chainId")).toInt() == chainId)
                return c.toObject();
        return {};
    }();
    ensureChainConfig(chainId, chain.value(QStringLiteral("rpcUrl")).toString());

    setStatusText(QStringLiteral("Testing chain %1…").arg(chainId));
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("verify_chain_id"), QJsonArray{ chainId },
        [this, chainId](const logos::web::ModuleCallResult& res) {
            if (!res.ok) {
                setStatusText(QStringLiteral("chain %1: %2").arg(chainId).arg(res.error));
                return;
            }
            const QJsonValue reply =
                QJsonDocument::fromJson(res.value.toString().toUtf8()).object();
            const QJsonValue got = resultOf(reply);
            setStatusText(got.isNull()
                              ? QStringLiteral("chain %1: %2").arg(chainId).arg(errorOf(reply))
                              : QStringLiteral("chain %1 answered").arg(chainId));
        });
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("pending"), true);
    return jsonText(out);
}

// ── accounts: keystore_module ────────────────────────────────────────────────

void WalletUiWebBackend::refreshAccounts()
{
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("list_accounts"), QJsonArray{},
        [this](const logos::web::ModuleCallResult& res) {
            if (!res.ok) {
                setStatusText(QStringLiteral("keystore_module: %1").arg(res.error));
                return;
            }
            // The keystore answers a JSON TEXT, as every rust-first module on
            // this wire does; `{ok, accounts:[…]}` once parsed.
            const QJsonObject reply =
                QJsonDocument::fromJson(res.value.toString().toUtf8()).object();
            QJsonObject out;
            out.insert(QStringLiteral("accounts"),
                       reply.value(QStringLiteral("accounts")).toArray());
            setAccountsJson(jsonText(out));
        });
}

QString WalletUiWebBackend::createAccount(QString passphrase, QString label)
{
    Q_UNUSED(label)
    setStatusText(QStringLiteral("Creating account…"));
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("new_account"), QJsonArray{ passphrase },
        [this](const logos::web::ModuleCallResult& res) {
            if (!res.ok) {
                setStatusText(QStringLiteral("keystore_module: %1").arg(res.error));
                return;
            }
            setStatusText(QStringLiteral("Account created"));
            refreshAccounts();
        });
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("pending"), true);
    return jsonText(out);
}

QString WalletUiWebBackend::importMnemonic(QString phraseJson, QString label)
{
    Q_UNUSED(phraseJson)
    Q_UNUSED(label)
    return refuse(QStringLiteral("Mnemonic import"), kKeystore);
}

bool WalletUiWebBackend::unlock(QString address, QString passphrase)
{
    setStatusText(QStringLiteral("Unlocking…"));
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("unlock"), QJsonArray{ address, passphrase },
        [this](const logos::web::ModuleCallResult& res) {
            const bool ok = res.ok && res.value.toBool();
            setAccountUnlocked(ok);
            setStatusText(ok ? QStringLiteral("Unlocked")
                             : QStringLiteral("Wrong passphrase"));
        });
    return true;
}

bool WalletUiWebBackend::lock(QString address)
{
    logos::web::callModuleAsync(kKeystore, QStringLiteral("lock"),
                                QJsonArray{ address },
                                [](const logos::web::ModuleCallResult&) {});
    setAccountUnlocked(false);
    setStatusText(QStringLiteral("Locked"));
    return true;
}

// ── balances: the Bundled eth_rpc_module ─────────────────────────────────────
//
// THE CRITERION, in one fan-out. One `eth_getBalance` per configured chain,
// each an independent call over the door; the replies land in whatever order
// the container answers and the aggregate is published when the last one is
// in. A counter rather than a join: there is one event loop, so the decrement
// and the publish cannot interleave with each other.
void WalletUiWebBackend::refreshBalances(QString address)
{
    if (address.isEmpty()) {
        setStatusText(QStringLiteral("Pick an account first"));
        return;
    }
    m_balanceAddress = address;
    m_balances = QJsonObject();
    m_balancePending = 0;
    setStatusText(QStringLiteral("Refreshing balances…"));

    for (const QJsonValue& c : m_chains) {
        const QJsonObject o = c.toObject();
        const int chainId = o.value(QStringLiteral("chainId")).toInt();
        ensureChainConfig(chainId, o.value(QStringLiteral("rpcUrl")).toString());
        ++m_balancePending;
        fetchBalance(address, chainId,
                     o.value(QStringLiteral("nativeSymbol")).toString());
    }
    if (m_balancePending == 0)
        publishBalances();
}

void WalletUiWebBackend::fetchBalance(const QString& address, int chainId,
                                      const QString& symbol)
{
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("get_balance"), QJsonArray{ chainId, address },
        [this, address, chainId, symbol](const logos::web::ModuleCallResult& res) {
            // A SECOND ASK SUPERSEDES THE FIRST. A reply for an address the
            // view has moved on from is dropped rather than merged, or the
            // aggregate would carry two accounts' balances under one name.
            if (address != m_balanceAddress)
                return;

            QJsonObject entry;
            entry.insert(QStringLiteral("chainId"), chainId);
            if (!res.ok) {
                entry.insert(QStringLiteral("native"),
                             QStringLiteral("unavailable (%1)").arg(res.error));
            } else {
                const QJsonObject reply =
                    QJsonDocument::fromJson(res.value.toString().toUtf8()).object();
                const QJsonValue wei = resultOf(reply);
                entry.insert(QStringLiteral("native"),
                             wei.isString()
                                 ? QStringLiteral("%1 %2").arg(wei.toString(), symbol)
                                 : QStringLiteral("unavailable (%1)").arg(errorOf(reply)));
            }
            entry.insert(QStringLiteral("tokens"), QJsonArray{});
            m_balances.insert(QString::number(chainId), entry);

            if (--m_balancePending <= 0)
                publishBalances();
        });
}

void WalletUiWebBackend::publishBalances()
{
    // Back into chain order, which is the order the view renders: the replies
    // arrive in whatever order the chains answered and a list that reshuffles
    // itself between refreshes is a UI bug with no error anywhere.
    QJsonArray chains;
    for (const QJsonValue& c : m_chains) {
        const QString key =
            QString::number(c.toObject().value(QStringLiteral("chainId")).toInt());
        if (m_balances.contains(key))
            chains.append(m_balances.value(key));
    }
    QJsonObject inner;
    inner.insert(QStringLiteral("chains"), chains);
    QJsonObject root;
    root.insert(QStringLiteral("balances"), inner);
    setBalancesJson(jsonText(root));
    setStatusText(QStringLiteral("Balances updated"));
}

// ── everything the coordinator owns ──────────────────────────────────────────

void WalletUiWebBackend::loadTokens(int chainId)
{
    Q_UNUSED(chainId)
    refuse(QStringLiteral("Token lists"), QStringLiteral("token_list_module"));
    setTokensJson(QStringLiteral("{\"tokens\":[]}"));
}

bool WalletUiWebBackend::addCustomToken(QString tokenJson)
{
    Q_UNUSED(tokenJson)
    refuse(QStringLiteral("Custom tokens"), QStringLiteral("token_list_module"));
    return false;
}

void WalletUiWebBackend::refreshMarket(QString address)
{
    Q_UNUSED(address)
    refuse(QStringLiteral("Market prices"), QStringLiteral("uniswap_module"));
    setMarketJson(QStringLiteral("{\"chains\":[]}"));
}

QString WalletUiWebBackend::estimateFee(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuse(QStringLiteral("Fee estimation"),
                  QStringLiteral("wallet_backend_module"));
}

QString WalletUiWebBackend::sendNative(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuse(QStringLiteral("Sending"), QStringLiteral("wallet_backend_module"));
}

QString WalletUiWebBackend::sendErc20(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuse(QStringLiteral("Sending"), QStringLiteral("wallet_backend_module"));
}

void WalletUiWebBackend::refreshHistory(QString address)
{
    Q_UNUSED(address)
    refuse(QStringLiteral("History"), QStringLiteral("wallet_backend_module"));
    setHistoryJson(QStringLiteral("{\"history\":[]}"));
}

QString WalletUiWebBackend::initPrivate(QString address, int chainId)
{
    Q_UNUSED(address)
    Q_UNUSED(chainId)
    return refuse(QStringLiteral("Private accounts"), QStringLiteral("railgun_module"));
}

void WalletUiWebBackend::syncPrivate()
{
    refuse(QStringLiteral("Private accounts"), QStringLiteral("railgun_module"));
}

void WalletUiWebBackend::refreshShieldedBalance()
{
    refuse(QStringLiteral("Private accounts"), QStringLiteral("railgun_module"));
}

QString WalletUiWebBackend::shield(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuse(QStringLiteral("Shielding"), QStringLiteral("railgun_module"));
}

QString WalletUiWebBackend::privateSend(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuse(QStringLiteral("Private send"), QStringLiteral("railgun_module"));
}
