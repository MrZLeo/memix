class Memix < Formula
  desc "Immutable local memory and context store CLI"
  homepage "https://github.com/MrZLeo/memix"
  url "https://github.com/MrZLeo/memix.git", branch: "master"
  version "0.4.0"
  license "Unlicense"
  head "https://github.com/MrZLeo/memix.git", branch: "master"

  depends_on "cmake" => :build
  depends_on "ninja" => :build

  resource "cpm" do
    url "https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.42.0/CPM.cmake"
    sha256 "2020b4fc42dba44817983e06342e682ecfc3d2f484a581f11cc5731fbe4dce8a"
  end

  resource "simdjson" do
    url "https://github.com/simdjson/simdjson/archive/refs/tags/v4.6.2.tar.gz"
    sha256 "c240d4bffcccda4fe3a2bba2872718d96fc92e56d2615bfac4f9b2bad89a6386"
  end

  resource "argum" do
    url "https://github.com/gershnik/argum/archive/refs/tags/v2.8.tar.gz"
    sha256 "2f54b31c0afd480ba46e0c8835014db33e6d89455ec5e1f009c1f30b691f5c95"
  end

  resource "spdlog" do
    url "https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz"
    sha256 "d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744"
  end

  def install
    cpm_cache = buildpath/".cpm-cache"

    (buildpath/"build/cmake").mkpath
    resource("cpm").stage do
      cp "CPM.cmake", buildpath/"build/cmake/CPM_0.42.0.cmake"
    end
    resource("simdjson").stage cpm_cache/"simdjson/4924"
    resource("argum").stage cpm_cache/"argum/e544"
    resource("spdlog").stage cpm_cache/"spdlog/7d8f"

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
    assert_match "memix #{version}", shell_output("#{bin}/memix-store --version")

    (testpath/"input.txt").write "hello from homebrew\n"
    store_root = testpath/"store"
    system bin/"memix-store", "--store-root", store_root, "ingest",
           "--type", "rules", "--dest", "input.txt", testpath/"input.txt"

    output = shell_output("#{bin}/memix-store --store-root #{store_root} list --type rules")
    assert_match "rules", output
    assert_match "input.txt", output
    assert_match "memix #{version}", shell_output("#{bin}/memix-view --version")
  end
end
