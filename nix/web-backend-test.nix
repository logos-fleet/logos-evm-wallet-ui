# THE `web` VARIANT'S BACKEND, DRIVEN — what this wallet asks for, of whom, and
# in what order.
#
# WHY IT IS NOT A BUILD OF THE `web` VARIANT. The wasm image is a page inside a
# webview: there is no process to attach to and no container here to answer it.
# What broke in logos-workspace#147 needed neither — `importMnemonic` answered
# "Mnemonic import needs keystore_module, which has no mobile build" while the
# keystore was loaded and answering `list_accounts` in the same run, and NO CALL
# WAS EVER MADE. The defect is in the asking, so the asking is what is checked:
# the shipped translation unit, compiled natively, with the one door it reaches
# the container through replaced by a recorder (tests/fake_door.cpp).
#
# WHAT IT THEREFORE DOES NOT SAY. It is not the emscripten toolchain and not a
# container, so it says nothing about wasm-specific behaviour, about the page's
# bridge, or about whether the keystore's OWN `web` variant answers — that last
# one is keystore_module's `web-variant` check, which drives a real image.
{ pkgs, src, door }:

pkgs.stdenv.mkDerivation {
  name = "wallet-ui-web-backend-tests";
  inherit src;

  nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.qt6.qtbase pkgs.qt6.qtremoteobjects ];
  buildInputs = [ pkgs.qt6.qtbase pkgs.qt6.qtremoteobjects ];

  # Nothing here is installed or run later: the binary runs in this build and
  # what comes out is the transcript. A Qt wrapper would only add a launcher for
  # a binary nobody launches.
  dontWrapQtApps = true;

  configurePhase = ''
    runHook preConfigure
    cmake -S tests -B build -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLOGOS_WEB_DOOR_DIR=${door}
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    cmake --build build
    runHook postBuild
  '';

  # THE TRANSCRIPT IS THE OUTPUT. Each case prints one PASS line naming what
  # held; a failure prints what the backend asked for instead and exits 1, which
  # fails the derivation.
  #
  # REDIRECTED, NEVER PIPED: a `| tee` would report the pipe's status and a
  # failing drive would land as a passing check.
  installPhase = ''
    runHook preInstall
    mkdir -p $out
    if ./build/web_backend_test > $out/result 2>&1; then
      cat $out/result
    else
      cat $out/result
      exit 1
    fi
    runHook postInstall
  '';
}
