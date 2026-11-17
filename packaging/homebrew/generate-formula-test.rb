# frozen_string_literal: true

require "minitest/autorun"
require "tmpdir"

require_relative "generate-formula"

class GenerateFormulaTest < Minitest::Test
  SKILL_SHA256 = "a" * 64
  MACOS_ARM64_SHA256 = "b" * 64
  LINUX_X86_64_SHA256 = "c" * 64
  LICENSE_SHA256 = "d" * 64

  def setup
    @temporary_directory = Dir.mktmpdir("memix-homebrew-test")
    @checksums_path = File.join(@temporary_directory, "SHA256SUMS")
    File.write(
      @checksums_path,
      <<~CHECKSUMS,
        #{SKILL_SHA256}  memix-cli-skill.tar.gz
        #{MACOS_ARM64_SHA256}  memix-macos-arm64
        #{LINUX_X86_64_SHA256}  memix-linux-x86_64
        #{"e" * 64}  memix-windows-x86_64.exe
      CHECKSUMS
    )
  end

  def teardown
    FileUtils.remove_entry(@temporary_directory)
  end

  def test_renders_platform_artifacts_and_license
    formula = MemixHomebrew.render(
      "v1.2.3",
      @checksums_path,
      license_sha256: LICENSE_SHA256,
    )

    assert_includes formula, "class Memix < Formula"
    assert_includes formula, 'license "BSD-3-Clause"'
    assert_includes formula, "releases/download/v1.2.3/memix-cli-skill.tar.gz"
    assert_includes formula, "releases/download/v1.2.3/memix-macos-arm64"
    assert_includes formula, "releases/download/v1.2.3/memix-linux-x86_64"
    assert_includes formula, "raw.githubusercontent.com/MrZLeo/memix/v1.2.3/LICENSE"
    assert_includes formula, %(sha256 "#{SKILL_SHA256}")
    assert_includes formula, %(sha256 "#{MACOS_ARM64_SHA256}")
    assert_includes formula, %(sha256 "#{LINUX_X86_64_SHA256}")
    assert_includes formula, %(sha256 "#{LICENSE_SHA256}")
    refute_match(/@[A-Z0-9_]+@/, formula)
  end

  def test_renders_v0_7_0_fixture_with_git_license
    formula = MemixHomebrew.render(
      "v0.7.0",
      File.join(__dir__, "testdata/v0.7.0-SHA256SUMS"),
    )

    assert_includes formula, "releases/download/v0.7.0/memix-macos-arm64"
    assert_includes formula, "releases/download/v0.7.0/memix-linux-x86_64"
    refute_match(/@[A-Z0-9_]+@/, formula)
  end

  def test_rejects_non_stable_tag
    error = assert_raises(ArgumentError) do
      MemixHomebrew.render(
        "v1.2.3-rc.1",
        @checksums_path,
        license_sha256: LICENSE_SHA256,
      )
    end

    assert_match "stable semantic version", error.message
  end

  def test_requires_platform_checksums
    %w[memix-cli-skill.tar.gz memix-macos-arm64 memix-linux-x86_64].each do |missing|
      lines = File.readlines(@checksums_path).reject { |line| line.include?(missing) }
      partial_path = File.join(@temporary_directory, "SHA256SUMS-#{missing}")
      File.write(partial_path, lines.join)

      error = assert_raises(ArgumentError) do
        MemixHomebrew.render(
          "v1.2.3",
          partial_path,
          license_sha256: LICENSE_SHA256,
        )
      end

      assert_match missing, error.message
    end
  end
end
