{
  description = "Logos multi-chain EVM wallet UI (QML, Metamask-like) over wallet_backend_module.";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # The backend the UI drives. Declaring its whole dependency tree here is what
    # lets the standalone app bundle and auto-load every module in order.
    wallet_backend_module.url = "github:logos-co/logos-evm-wallet-backend-module/5ed06f701622389658d6b55d1b5c5a2bf6cce508";

    # Each leaf is the backend's OWN locked input, never a second pin of our own:
    # collectAllModuleDeps is `transitive // direct`, so a url here shadows the
    # backend's lock and ships it a module it was not built against.
    eth_rpc_module.follows = "wallet_backend_module/eth_rpc_module";
    keystore_module.follows = "wallet_backend_module/keystore_module";
    token_list_module.follows = "wallet_backend_module/token_list_module";
    uniswap_module.follows = "wallet_backend_module/uniswap_module";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
