# Homebrew formula for leansip. This repository doubles as its tap:
#
#   brew tap arjan/leansip https://github.com/arjan/leansip
#   brew install leansip          # the release named by `tag:` below
#   brew install --HEAD leansip   # main
#
# leansip needs baresip with opus and sndfile (recording, the dialer's activity display), which
# Homebrew's own baresip formula leaves out, so this formula builds a private baresip into
# libexec with every module leansip uses compiled in, and points leansipd at it.
class Leansip < Formula
  desc "Keyboard-driven SIP dialer and agent CLI on top of baresip"
  homepage "https://github.com/arjan/leansip"
  # A tag, not a tarball: a checksum of the release tarball cannot live inside that tarball.
  # Bump together with project(leansip VERSION ...) in CMakeLists.txt.
  url "https://github.com/arjan/leansip.git", tag: "v0.5.0"
  license "BSD-3-Clause"
  head "https://github.com/arjan/leansip.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "pkgconf" => :build
  depends_on "tomlplusplus" => :build
  depends_on "libsndfile"
  depends_on "openssl@4"
  depends_on "opus"
  depends_on "qtbase"

  uses_from_macos "zlib"

  on_linux do
    depends_on "alsa-lib"
  end

  # re and baresip are released together; keep the versions equal.
  resource "re" do
    url "https://github.com/baresip/re/archive/refs/tags/v4.13.0.tar.gz"
    sha256 "3e62b66cb833f22561ec3351647efb3c488766978d72f56de1039a7e5ddf70a9"
  end

  resource "baresip" do
    url "https://github.com/baresip/baresip/archive/refs/tags/v4.13.0.tar.gz"
    sha256 "688dbb92844340c2dd8dab7d93aaebccb3aa433af9215bb099ca598fafc0e0a2"
  end

  def install
    deps = buildpath/"baresip-deps"
    audio = OS.mac? ? "coreaudio" : "alsa"
    modules = %W[
      account #{audio} auconv aufile auresamp contact ctrl_tcp debug_cmd dtls_srtp g711 libg722 g722
      ice menu opus sndfile srtp stun turn uuid
    ].join(";")

    resource("re").stage do
      system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
             "-DLIBRE_BUILD_SHARED=OFF", "-DLIBRE_BUILD_STATIC=ON",
             "-DOPENSSL_ROOT_DIR=#{Formula["openssl@4"].opt_prefix}",
             *std_cmake_args(install_prefix: deps)
      system "cmake", "--build", "build"
      system "cmake", "--install", "build"
    end

    resource("baresip").stage do
      system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
             "-DSTATIC=ON", "-DMODULES=#{modules}",
             "-DCMAKE_PREFIX_PATH=#{deps}", "-DRE_INCLUDE_DIR=#{deps}/include/re",
             "-DOPENSSL_ROOT_DIR=#{Formula["openssl@4"].opt_prefix}",
             *std_cmake_args(install_prefix: deps)
      system "cmake", "--build", "build"
      system "cmake", "--install", "build"
    end
    (libexec/"bin").install deps/"bin/baresip"
    (libexec/"sounds").install Dir[deps/"share/baresip/*.wav"]

    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           "-DLEANSIP_BUILD_TESTS=OFF", "-DLEANSIP_WERROR=OFF",
           "-DLEANSIP_BARESIP_PATH=#{libexec}/bin/baresip",
           "-DLEANSIP_BARESIP_SOUNDS=#{libexec}/sounds",
           *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  def caveats
    <<~EOS
      Accounts go in ~/.config/leansip/leansip.toml (chmod 600); see
        #{doc}/configuration.md
      The first call asks for microphone access for your terminal (or whatever started leansip).
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/leansipctl --version") unless build.head?
    system bin/"leansipd", "--version"
    # Every module leansip loads is compiled into its baresip.
    (testpath/"config").write <<~EOS
      module opus.so
      module g722.so
      module sndfile.so
      module_app menu.so
      module_app ctrl_tcp.so
      ctrl_tcp_listen 127.0.0.1:0
    EOS
    output = shell_output("#{libexec}/bin/baresip -f #{testpath} -t 1 2>&1")
    assert_match "baresip is ready", output
    refute_match(/^module \S+: /, output) # "module nope.so: No such file or directory"
  end
end
