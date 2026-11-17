class Memix < Formula
  desc "Immutable local memory and context store CLI"
  homepage "https://github.com/MrZLeo/memix"
  url "https://github.com/MrZLeo/memix.git", branch: "master"
  version "0.7.0"
  license "Unlicense"
  head "https://github.com/MrZLeo/memix.git", branch: "master"

  depends_on "cmake" => :build
  depends_on "ninja" => :build

  on_macos do
    depends_on macos: :tahoe
  end

  resource "cpm" do
    url "https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.42.0/CPM.cmake"
    sha256 "2020b4fc42dba44817983e06342e682ecfc3d2f484a581f11cc5731fbe4dce8a"
  end

  resource "simdjson" do
    url "https://github.com/simdjson/simdjson/archive/refs/tags/v5.0.3.tar.gz"
    sha256 "295e96d96d6d55face58e99b47ddc1f6c7c1a302f2ce5edf2c1ba7518972f0ed"
  end

  resource "argum" do
    url "https://github.com/gershnik/argum/archive/refs/tags/v2.8.tar.gz"
    sha256 "2f54b31c0afd480ba46e0c8835014db33e6d89455ec5e1f009c1f30b691f5c95"
  end

  resource "spdlog" do
    url "https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz"
    sha256 "d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744"
  end

  resource "ftxui" do
    url "https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.2.tar.gz"
    sha256 "28da2c3389440af869f8781679b537c4a3d5b4df42aeab54eb1564d1b61af864"
  end

  def install
    cpm_cache = buildpath/".cpm-cache"

    (buildpath/"build/cmake").mkpath
    resource("cpm").stage do
      cp "CPM.cmake", buildpath/"build/cmake/CPM_0.42.0.cmake"
    end
    resource("simdjson").stage cpm_cache/"simdjson/4606"
    resource("argum").stage cpm_cache/"argum/e544"
    resource("spdlog").stage cpm_cache/"spdlog/7d8f"
    resource("ftxui").stage cpm_cache/"ftxui/7e12"

    args = std_cmake_args + %W[
      -DCPM_SOURCE_CACHE=#{cpm_cache}
      -Dmemix_BUILD_MACOS_INSPECTOR=OFF
      -Dmemix_ENABLE_UNIT_TESTING=OFF
    ]

    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja", *args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_match "memix #{version}", shell_output("#{bin}/memix --version")

    (testpath/"input.txt").write "hello from homebrew\n"
    store_root = testpath/"store"
    system bin/"memix", "store", "--store-root", store_root, "ingest",
           "--type", "rules", "--dest", "input.txt", testpath/"input.txt"

    output = shell_output("#{bin}/memix store --store-root #{store_root} list --type rules")
    assert_match "rules", output
    assert_match "input.txt", output
    assert_match "Usage: memix view", shell_output("#{bin}/memix view --help")
    assert_match "Usage: memix tui", shell_output("#{bin}/memix tui --help")
    %w[store view tui].each do |command|
      assert_match "memix #{version}", shell_output("#{bin}/memix #{command} --version")
    end
  end
end
