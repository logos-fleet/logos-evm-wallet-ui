import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import Logos.Controls
import Logos.Theme

// Metamask-like multi-chain EVM wallet view. Drives the C++ backend
// (wallet_ui.rep) over QtRO: PROPs (backend.*Json) auto-sync from the backend,
// SLOTs are called directly (PROP-updating) or via logos.watch (for a reply).
// The backend in turn calls wallet_backend_module over the typed modules() client.
//
// Styled with the Logos design system (Logos.Controls + Logos.Theme) so it
// matches the other plugins; the standalone host bundles those QML modules on
// the import path. Colours/typography/spacing come from the Theme singleton.
//
// The sections live on a TabBar + StackLayout. Only the active page's controls
// are in the visible scene, so headless qt-mcp drives navigation through the
// root's `selectTab(i)` helper (call_method on objectName "walletRoot") before
// asserting a tab's controls — the account selector + status line stay shared
// above/below the tabs so they're always visible.
Item {
    id: root
    objectName: "walletRoot"
    width: 460
    height: 760

    readonly property var backend: logos.module("wallet_ui")
    property bool ready: false
    // When a custom network is saved/selected on the Advanced tab, sends target it
    // directly (so a freshly-added local node is usable without touching the Send
    // dropdown). Cleared when the user picks a chain from the Send dropdown.
    property int overrideChainId: 0

    // Parsed views of the backend's JSON PROPs.
    readonly property var chains: parseField(backend ? backend.chainsJson : "", "chains", [])
    readonly property var accounts: parseField(backend ? backend.accountsJson : "", "accounts", [])
    readonly property var balances: parseField(backend ? backend.balancesJson : "", "balances", ({}))
    readonly property var tokens: parseField(backend ? backend.tokensJson : "", "tokens", [])
    readonly property var history: parseField(backend ? backend.historyJson : "", "history", [])
    readonly property var market: parseField(backend ? backend.marketJson : "", "chains", [])

    function parseField(json, field, fallback) {
        if (!json) return fallback
        try { var o = JSON.parse(json); return (o && o[field] !== undefined) ? o[field] : fallback }
        catch (e) { return fallback }
    }

    // WHAT THIS VIEW IS RENDERING, on the console, the moment it renders it.
    //
    // A `web` variant draws into a canvas: from outside the page there is no
    // scene graph to query and no label to read, and a host driving it can see
    // only what the page says about itself (the container forwards a page's
    // console to the app's log). So the one state this module exists to show —
    // the balances, and which account they are for — is announced where a
    // device run can read it. Harmless on the desktop, where it is the same
    // line in the same place.
    onBalancesChanged: {
        if (balances && balances.chains && balances.chains.length > 0)
            console.log("wallet_ui: rendering balances for "
                        + (backend ? backend.selectedAccount : "?")
                        + " -- " + JSON.stringify(balances.chains))
    }

    // Doctest hook: switch the active tab deterministically. qt-mcp drives this via
    // `call_method` (find_by objectName "walletRoot", method "selectTab", args [i]),
    // the same pattern the tutorial QML UI uses for coreModulesView.openInterface.
    // Switching the tab makes that page's controls visible/findable to qt-mcp.
    function selectTab(i) { tabs.currentIndex = Number(i) }

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "wallet_ui") root.ready = isReady && root.backend !== null
        }
    }
    Component.onCompleted: root.ready = root.backend !== null && logos.isViewModuleReady("wallet_ui")

    // Themed background.
    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    // Drives a raised signing request to completion. The decision itself happens
    // in the Signer app; this only asks the backend whether it has landed yet.
    Timer {
        interval: 1000
        repeat: true
        running: root.ready && backend && backend.pendingRequestId !== ""
        onTriggered: backend.sendStatus(backend.pendingRequestId)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.medium
        spacing: Theme.spacing.small

        // ── Header ──
        LogosText {
            text: "Logos Wallet"
            font.pixelSize: Theme.typography.titleText
            font.weight: Theme.typography.weightBold
            color: Theme.palette.text
        }
        LogosText {
            text: root.ready ? "Connected to backend" : "Connecting to backend…"
            color: root.ready ? Theme.palette.success : Theme.palette.warning
            font.pixelSize: Theme.typography.secondaryText
        }

        // ── Accounts (shared across tabs) ──
        LogosText { text: "Accounts"; font.weight: Theme.typography.weightBold; Layout.topMargin: Theme.spacing.tiny }
        RowLayout {
            Layout.fillWidth: true
            LogosComboBox {
                id: acctBox; Layout.fillWidth: true; model: root.accounts
                placeholderText: "No accounts"
                onActivated: backend.selectedAccount = root.accounts[currentIndex]
                // Auto-select the first account once accounts load, so backend
                // events (balances/history) target it without a manual pick.
                onCountChanged: if (count > 0 && backend
                        && (!backend.selectedAccount || backend.selectedAccount.length === 0)) {
                    currentIndex = 0
                    backend.selectedAccount = root.accounts[0]
                }
            }
            // There is no unlock any more: this wallet never holds a vault
            // password. When a send needs a signature, the human approves it in
            // the Signer app; this only reports that one is waiting.
            LogosText {
                visible: root.ready && backend && backend.pendingRequestId !== ""
                text: "Waiting for approval in the Signer app"
                textFormat: Text.PlainText
            }
            LogosButton { text: "New"; enabled: root.ready; onClicked: createDialog.open() }
        }

        // ── Tabs ──
        LogosTabBar {
            id: tabs
            objectName: "walletTabs"
            Layout.fillWidth: true
            LogosTabButton { text: "Balances" }
            LogosTabButton { text: "Market" }
            LogosTabButton { text: "Send" }
            LogosTabButton { text: "Tokens" }
            LogosTabButton { text: "History" }
            LogosTabButton { text: "Settings" }
            LogosTabButton { text: "Advanced" }
        }

        StackLayout {
            id: pages
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ── 0 · Balances ──
            LogosScrollView {
                clip: true
                ColumnLayout {
                    width: pages.width - 16
                    spacing: Theme.spacing.small
                    RowLayout {
                        Layout.fillWidth: true
                        LogosText { text: "Balances"; font.weight: Theme.typography.weightBold; Layout.fillWidth: true }
                        LogosButton {
                            text: "Refresh balances"; enabled: root.ready && acctBox.currentText.length > 0
                            onClicked: backend.refreshBalances(acctBox.currentText)
                        }
                    }
                    Repeater {
                        model: root.balances && root.balances.chains ? root.balances.chains : []
                        LogosFrame {
                            Layout.fillWidth: true
                            ColumnLayout {
                                anchors.fill: parent
                                LogosText { text: "chain " + modelData.chainId; font.weight: Theme.typography.weightBold }
                                LogosText { text: "native: " + modelData.native }
                                Repeater {
                                    model: modelData.tokens || []
                                    LogosText { font.pixelSize: Theme.typography.secondaryText; color: Theme.palette.textSecondary; text: modelData.balance + "  " + modelData.address }
                                }
                            }
                        }
                    }
                }
            }

            // ── 1 · Market (Uniswap prices for held tokens, balance > 0) ──
            LogosScrollView {
                clip: true
                ColumnLayout {
                    width: pages.width - 16
                    spacing: Theme.spacing.small
                    RowLayout {
                        Layout.fillWidth: true
                        LogosText { text: "Market"; font.weight: Theme.typography.weightBold; Layout.fillWidth: true }
                        LogosButton {
                            text: "Refresh market"; enabled: root.ready && acctBox.currentText.length > 0
                            onClicked: backend.refreshMarket(acctBox.currentText)
                        }
                    }
                    Repeater {
                        model: root.market
                        ColumnLayout {
                            Layout.fillWidth: true
                            LogosText { text: "chain " + modelData.chainId; font.pixelSize: Theme.typography.secondaryText; color: Theme.palette.textTertiary }
                            Repeater {
                                model: modelData.items || []
                                RowLayout {
                                    Layout.fillWidth: true
                                    LogosText { text: modelData.symbol; font.weight: Theme.typography.weightBold; Layout.preferredWidth: 64 }
                                    LogosText {
                                        Layout.fillWidth: true; font.pixelSize: Theme.typography.secondaryText; color: Theme.palette.textSecondary
                                        text: modelData.usd != null ? ("$" + Number(modelData.usd).toFixed(2)) : "—"
                                    }
                                    LogosText {
                                        font.pixelSize: Theme.typography.secondaryText; color: Theme.palette.success
                                        text: modelData.valueUsd != null ? ("$" + Number(modelData.valueUsd).toFixed(2)) : ""
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── 2 · Send ──
            LogosScrollView {
                clip: true
                ColumnLayout {
                    width: pages.width - 16
                    spacing: Theme.spacing.small
                    LogosText { text: "Send"; font.weight: Theme.typography.weightBold }
                    LogosComboBox {
                        id: sendChain; Layout.fillWidth: true; textRole: "name"; model: root.chains
                        placeholderText: "Network"
                        // Picking a chain here clears the Advanced-tab override.
                        onActivated: root.overrideChainId = 0
                    }
                    LogosText {
                        visible: root.overrideChainId > 0; color: Theme.palette.success; font.pixelSize: Theme.typography.secondaryText
                        text: "Active network: chain " + root.overrideChainId + " (set on Advanced)"
                    }
                    LogosCheckbox { id: isErc20; text: "ERC20 token" }
                    LogosTextField { id: tokenAddr; Layout.fillWidth: true; visible: isErc20.checked; placeholderText: "Token contract address" }
                    LogosTextField { id: toAddr; objectName: "sendToField"; Layout.fillWidth: true; placeholderText: "Recipient address (0x…)" }
                    LogosTextField { id: amount; objectName: "sendAmountField"; Layout.fillWidth: true; placeholderText: "Amount (base units / wei)" }
                    RowLayout {
                        LogosButton {
                            text: "Estimate"; enabled: root.ready
                            onClicked: logos.watch(backend.estimateFee(JSON.stringify(buildSend())),
                                                   function (r) { feePreview.text = "fee: " + r },
                                                   function (e) { feePreview.text = "estimate failed" })
                        }
                        LogosButton {
                            text: "Send transaction"; enabled: root.ready && backend && backend.pendingRequestId === ""
                            onClicked: {
                                var m = isErc20.checked ? backend.sendErc20(JSON.stringify(buildSend()))
                                                        : backend.sendNative(JSON.stringify(buildSend()))
                                logos.watch(m, function (r) {}, function (e) {})
                            }
                        }
                    }
                    LogosText { id: feePreview; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                }
            }

            // ── 3 · Tokens ──
            LogosScrollView {
                clip: true
                ColumnLayout {
                    width: pages.width - 16
                    spacing: Theme.spacing.small
                    RowLayout {
                        Layout.fillWidth: true
                        LogosText { text: "Tokens"; font.weight: Theme.typography.weightBold; Layout.fillWidth: true }
                        LogosButton {
                            text: "Load tokens"; enabled: root.ready
                            onClicked: backend.loadTokens(root.chains.length ? root.chains[sendChain.currentIndex].chainId : 1)
                        }
                    }
                    Repeater {
                        model: root.tokens
                        LogosText { font.pixelSize: Theme.typography.secondaryText; text: (modelData.symbol || "?") + "  " + (modelData.name || "") + "  " + modelData.address }
                    }
                    LogosButton { text: "Add custom token"; enabled: root.ready; onClicked: addTokenDialog.open() }
                }
            }

            // ── 4 · History ──
            LogosScrollView {
                clip: true
                ColumnLayout {
                    width: pages.width - 16
                    spacing: Theme.spacing.small
                    RowLayout {
                        Layout.fillWidth: true
                        LogosText { text: "Recent activity"; font.weight: Theme.typography.weightBold; Layout.fillWidth: true }
                        LogosButton {
                            text: "Refresh history"; enabled: root.ready && acctBox.currentText.length > 0
                            onClicked: backend.refreshHistory(acctBox.currentText)
                        }
                    }
                    LogosText {
                        objectName: "historyEmpty"
                        visible: !root.history || root.history.length === 0
                        text: "No transactions yet"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.secondaryText
                    }
                    Repeater {
                        model: root.history
                        RowLayout {
                            Layout.fillWidth: true
                            // Separate labels so each field is its own text node
                            // (kind/status are assertable verbatim in UI tests).
                            LogosText { text: modelData.kind; font.weight: Theme.typography.weightBold; font.pixelSize: Theme.typography.secondaryText; Layout.preferredWidth: 64 }
                            LogosText {
                                text: modelData.status; font.pixelSize: Theme.typography.secondaryText
                                color: modelData.status === "confirmed" ? Theme.palette.success : (modelData.status === "failed" ? Theme.palette.error : Theme.palette.warning)
                            }
                            LogosText { text: modelData.hash; font.pixelSize: Theme.typography.secondaryText; Layout.fillWidth: true; elide: Text.ElideMiddle; color: Theme.palette.textSecondary }
                        }
                    }
                }
            }

            // ── 5 · Settings (privacy / proxy) ──
            LogosScrollView {
                clip: true
                ColumnLayout {
                    width: pages.width - 16
                    spacing: Theme.spacing.small
                    LogosText { text: "Privacy / proxy"; font.weight: Theme.typography.weightBold }
                    LogosTextField { id: proxyUrl; Layout.fillWidth: true; placeholderText: "socks5h://127.0.0.1:9050" }
                    LogosCheckbox { id: proxyRequired; text: "Require proxy (fail-closed)" }
                    LogosButton {
                        text: "Apply proxy"; enabled: backend !== null
                        onClicked: backend.setProxyConfig(JSON.stringify({
                            proxy: proxyUrl.text.length ? proxyUrl.text : null,
                            proxyRequired: proxyRequired.checked
                        }))
                    }
                }
            }

            // ── 6 · Advanced (custom networks + account import; dev / testing) ──
            LogosScrollView {
                clip: true
                ColumnLayout {
                    width: pages.width - 16
                    spacing: Theme.spacing.small

                    // Custom network — add or replace an RPC endpoint (e.g. a local
                    // dev node). Saving repoints the wallet at this chain (set_chains
                    // → eth-rpc) and makes it the active network for sends.
                    LogosText { text: "Custom network"; font.weight: Theme.typography.weightBold }
                    LogosText {
                        Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Theme.typography.secondaryText; color: Theme.palette.textTertiary
                        text: "Add or update an RPC endpoint — e.g. a local node at http://127.0.0.1:8545. " +
                              "Saving repoints the wallet and makes it the active send network."
                    }
                    LogosTextField { id: advChainId; objectName: "advChainIdField"; Layout.fillWidth: true; placeholderText: "Chain ID (e.g. 31337)" }
                    LogosTextField { id: advChainName; objectName: "advChainNameField"; Layout.fillWidth: true; placeholderText: "Network name (e.g. Local Anvil)" }
                    LogosTextField { id: advRpcUrl; objectName: "advRpcUrlField"; Layout.fillWidth: true; placeholderText: "RPC URL (http://127.0.0.1:8545)" }
                    LogosTextField { id: advSymbol; objectName: "advSymbolField"; Layout.fillWidth: true; placeholderText: "Native symbol (e.g. ETH)" }
                    LogosTextField { id: advMulticall; objectName: "advMulticallField"; Layout.fillWidth: true; placeholderText: "Multicall3 address (optional, 0x…)" }
                    RowLayout {
                        LogosButton {
                            text: "Test endpoint"; enabled: root.ready && advChainId.text.length > 0 && advRpcUrl.text.length > 0
                            // Push the entered endpoint into eth-rpc first (idempotent), then verify
                            // it — so Test works on a not-yet-saved network (verify what you typed).
                            // backend calls are serialized, so the push lands before the verify.
                            onClicked: {
                                backend.setChains(JSON.stringify(root.upsertChain()))
                                logos.watch(backend.testEndpoint(parseInt(advChainId.text)),
                                    function (r) {
                                        var ok = false; try { ok = JSON.parse(r).ok === true } catch (e) {}
                                        advResult.text = ok ? "Endpoint reachable" : ("Endpoint error: " + r)
                                    },
                                    function (e) { advResult.text = "Endpoint error" })
                            }
                        }
                        LogosButton {
                            text: "Save chain"; enabled: root.ready && advChainId.text.length > 0 && advRpcUrl.text.length > 0
                            onClicked: {
                                backend.setChains(JSON.stringify(root.upsertChain()))
                                root.overrideChainId = parseInt(advChainId.text)
                            }
                        }
                    }
                    LogosText { id: advResult; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }

                    LogosText { text: "Configured networks"; font.weight: Theme.typography.weightBold; Layout.topMargin: Theme.spacing.small }
                    Repeater {
                        model: root.chains
                        LogosText {
                            Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: Theme.typography.secondaryText; color: Theme.palette.textSecondary
                            text: modelData.chainId + " · " + modelData.name + " · " + modelData.rpcUrl
                        }
                    }

                    // Import an account from a seed phrase, then unlock it — inline
                    // (no modal) so the whole wallet flow is scriptable headless.
                    // Signing stays in the keystore module; the seed only transits
                    // to the backend's import call.
                    LogosText { text: "Import account (seed phrase)"; font.weight: Theme.typography.weightBold; Layout.topMargin: Theme.spacing.small }
                    LogosTextField { id: advSeed; objectName: "advSeedField"; Layout.fillWidth: true; placeholderText: "Seed phrase (BIP-39 words)" }
                    LogosTextField { id: advAcctLabel; objectName: "advAcctLabelField"; Layout.fillWidth: true; placeholderText: "Account label (e.g. main)" }
                    LogosTextField { id: advAcctPw; objectName: "advAcctPwField"; Layout.fillWidth: true; placeholderText: "Account passphrase"; echoMode: TextInput.Password }
                    RowLayout {
                        LogosButton {
                            text: "Import"; enabled: root.ready && advSeed.text.length > 0
                            onClicked: logos.watch(backend.importMnemonic(JSON.stringify({
                                    phrase: advSeed.text, accountIndex: 0, password: advAcctPw.text
                                }), advAcctLabel.text),
                                function (r) { advAcctResult.text = "Imported: " + r },
                                function (e) { advAcctResult.text = "Import failed: " + e })
                        }
                    }
                    LogosText { id: advAcctResult; Layout.fillWidth: true; elide: Text.ElideMiddle; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                }
            }
        }

        // ── Status (shared, always visible below the tabs) ──
        LogosText {
            Layout.fillWidth: true
            text: backend ? backend.statusText : ""; color: Theme.palette.textSecondary
        }
    }

    // Chain a send targets: the Advanced-tab override if set, else the Send tab's
    // dropdown selection.
    function activeSendChainId() {
        if (root.overrideChainId > 0) return root.overrideChainId
        return root.chains.length ? root.chains[sendChain.currentIndex].chainId : 1
    }

    function buildSend() {
        var p = { from: acctBox.currentText, to: toAddr.text, chainId: activeSendChainId(), amount: amount.text }
        if (isErc20.checked) p.tokenAddress = tokenAddr.text
        return p
    }

    // Build a ChainInfo (camelCase, matching wallet_backend's get/set_chains) from
    // the Advanced-tab form.
    function buildAdvChain() {
        var c = {
            chainId: parseInt(advChainId.text),
            name: advChainName.text.length ? advChainName.text : ("chain " + advChainId.text),
            rpcUrl: advRpcUrl.text,
            nativeSymbol: advSymbol.text.length ? advSymbol.text : "ETH"
        }
        if (advMulticall.text.length) c.multicall3 = advMulticall.text
        return c
    }

    // Upsert the Advanced-tab chain into the current chain list (replace by
    // chainId, else append). setChains replaces the whole list, so we preserve the
    // existing chains and add/update just this one.
    function upsertChain() {
        var cid = parseInt(advChainId.text)
        var out = []
        var found = false
        for (var i = 0; i < root.chains.length; i++) {
            if (root.chains[i].chainId === cid) { out.push(buildAdvChain()); found = true }
            else { out.push(root.chains[i]) }
        }
        if (!found) out.push(buildAdvChain())
        return out
    }

    // Modals — plain Dialog with a Theme-coloured surface + Logos inner content
    // (the basecamp pattern); LogosDialog uses left/rightActions rather than
    // standardButtons, so we keep Dialog here for the simple Ok/Cancel flow.
    Dialog {
        id: createDialog; title: "New account"; modal: true; anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        background: Rectangle { color: Theme.palette.backgroundSecondary; border.color: Theme.palette.borderSubtle; border.width: 1; radius: Theme.spacing.radiusLarge }
        ColumnLayout {
            LogosTextField { id: newLabel; placeholderText: "Label" }
            LogosTextField { id: newPw; placeholderText: "Passphrase"; echoMode: TextInput.Password }
        }
        onAccepted: { logos.watch(backend.createAccount(newPw.text, newLabel.text), function (r) {}, function (e) {}); newPw.text = "" }
    }
    Dialog {
        id: addTokenDialog; title: "Add custom token"; modal: true; anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        background: Rectangle { color: Theme.palette.backgroundSecondary; border.color: Theme.palette.borderSubtle; border.width: 1; radius: Theme.spacing.radiusLarge }
        ColumnLayout {
            LogosTextField { id: ctChain; placeholderText: "chainId" }
            LogosTextField { id: ctAddr; placeholderText: "Token address (0x…)" }
            LogosTextField { id: ctSym; placeholderText: "Symbol" }
            LogosTextField { id: ctDec; placeholderText: "Decimals" }
        }
        onAccepted: backend.addCustomToken(JSON.stringify({
            chainId: parseInt(ctChain.text), address: ctAddr.text,
            name: ctSym.text, symbol: ctSym.text, decimals: parseInt(ctDec.text)
        }))
    }
}
