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
    let
      nixpkgs = logos-module-builder.inputs.nixpkgs;
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];

      module = logos-module-builder.lib.mkLogosQmlModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
      };
    in
    module // {
      # ADDED TO the builder's checks, never in place of them: mkLogosQmlModule
      # publishes this module's integration test under the same attribute, and
      # replacing the set would drop it silently. Merged at BOTH levels for that
      # reason — the builder keys its checks over its own systems list, which
      # carries the x86_64-windows pseudo-system this check has no native
      # nixpkgs for, so those keys pass through untouched.
      #
      # `web-backend` drives the `web` variant's backend natively against a
      # recorded door — see nix/web-backend-test.nix for what that can and
      # cannot say.
      #
      # A SKIP THAT SAYS SO when the pinned logos-module-builder has no `wasm/`
      # directory. That header is the ONE thing this check needs from the
      # builder, and a pin from before the Wasm host existed does not have it —
      # the same pin that publishes no `web` variant for this module at all. A
      # check that failed there would be reporting a pin rollout as a defect;
      # one that was simply absent would be a green run with a missing test.
      checks = (module.checks or { }) // nixpkgs.lib.genAttrs systems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          door = "${logos-module-builder}/wasm";

          # ── WHAT THIS MODULE SHIPS vs WHAT IT DECLARES (#250) ──────────────
          #
          # `web-backend` below compiles the `web` backend's own translation
          # unit and asserts which modules it ASKS. This one asserts which
          # modules the artifact SAYS it needs -- the other half, and the one
          # that was wrong on a device: the shipped manifest named four modules
          # and no optional ones, so the core brought up four and
          # `wallet_backend_module` sat in the same app image unloaded while
          # three screens reported it missing.
          #
          # Every check in this tree reads metadata.json and therefore agrees
          # with itself. This one reads the BUILT package.
          #
          # A SKIP THAT SAYS SO, exactly as `web-backend` below does and for the
          # same pin: this repo's OWN flake.lock still points at a
          # logos-module-builder without `lib.checkWebManifest`, so from the
          # sub-repo flake there is no comparison to run. Omitting the attribute
          # was the first shape of this, and it is what a silent hole looks
          # like -- `ws test logos-evm-wallet-ui` reported a green run in which
          # nothing compared the manifest at all. A line naming the pin is what
          # tells a reader the difference between "checked" and "not checked
          # here".
          webManifestCheck =
            if (module.packages.${system} or { }) ? web
               && logos-module-builder.lib ? checkWebManifest
            then {
              web-manifest = logos-module-builder.lib.checkWebManifest pkgs {
                name = "wallet_ui";
                webPackage = module.packages.${system}.web;
                metadataFile = ./metadata.json;
              };
            }
            else {
              web-manifest = pkgs.runCommand "wallet-ui-web-manifest-check-skipped" { } ''
                echo "SKIP: web-manifest -- this logos-module-builder pin has no"
                echo "      lib.checkWebManifest, or publishes no \`web\` variant for"
                echo "      this module, so there is no shipped manifest to compare."
                echo "      Run through the workspace flake:"
                echo "      ws test logos-evm-wallet-ui --local logos-module-builder"
                mkdir -p $out
                echo skipped > $out/result
              '';
            };
        in
        (module.checks.${system} or { }) // {
          web-backend =
            if builtins.pathExists "${door}/logos_web_module_call.h"
            then import ./nix/web-backend-test.nix { inherit pkgs door; src = ./.; }
            else pkgs.runCommand "wallet-ui-web-backend-tests-skipped" { } ''
              echo "SKIP: web-backend -- this logos-module-builder pin has no"
              echo "      wasm/logos_web_module_call.h, so it publishes no \`web\`"
              echo "      variant for this module either. Run through the workspace"
              echo "      flake: ws test logos-evm-wallet-ui --local logos-module-builder"
              mkdir -p $out
              echo skipped > $out/result
            '';
        } // webManifestCheck);
    };
}
