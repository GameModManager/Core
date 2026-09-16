#include "engine/gmmpack/bsdiff.h"
#include "engine/gmmpack/codec.h"
#include "engine/gmmpack/patch.h"
#include "engine/gmmpack/sha256.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace engine::gmmpack;

namespace {

std::vector<uint8_t> bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::string str(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

// Build a patch-entry JSON doc the way the pack creator would.
std::string make_json(const std::string& mod, const std::vector<uint8_t>& old_data,
                      const std::vector<uint8_t>& new_data, const std::string& target,
                      int seq = 0) {
  std::vector<uint8_t> patch;
  std::string err;
  REQUIRE(bsdiff_create(old_data.data(), old_data.size(), new_data.data(),
                        new_data.size(), patch, err));
  std::string doc = "{\"modId\":\"" + mod + "\",";
  if (seq > 0) doc += "\"sequence\":" + std::to_string(seq) + ",";
  doc += "\"targetPath\":\"" + target + "\",";
  doc += "\"baseFileSha256\":\"" + sha256_hex(old_data) + "\",";
  doc += "\"algorithm\":\"bsdiff\",";
  doc += "\"payloadBase64\":\"" + base64_encode(patch) + "\"}";
  return doc;
}

void write_file(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("sha256 known vectors", "[gmmpack]") {
  CHECK(sha256_hex(bytes("")) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex(bytes("abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("base64 round-trip and strict rejects", "[gmmpack]") {
  for (size_t n = 0; n < 16; ++n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 37 + n);
    std::vector<uint8_t> back;
    CHECK(base64_decode(base64_encode(v), back));
    CHECK(back == v);
  }
  std::vector<uint8_t> all;
  for (int i = 0; i < 256; ++i) all.push_back(static_cast<uint8_t>(i));
  std::vector<uint8_t> back;
  CHECK(base64_decode(base64_encode(all), back));
  CHECK(back == all);

  std::vector<uint8_t> out;
  CHECK_FALSE(base64_decode("!!!", out));
  CHECK_FALSE(base64_decode("abc", out));      // bad length
  CHECK_FALSE(base64_decode("ab=c", out));     // padding mid-input
  CHECK_FALSE(base64_decode("a===bcd=", out)); // padding mid-input
  CHECK_FALSE(base64_decode("ab cd", out));    // whitespace rejected
  CHECK(base64_decode("", out));
  CHECK(out.empty());
}

TEST_CASE("bsdiff round-trips", "[gmmpack]") {
  std::string err;
  auto round_trip = [&](const std::vector<uint8_t>& old_data,
                        const std::vector<uint8_t>& new_data) {
    std::vector<uint8_t> patch, result;
    INFO("old=" << old_data.size() << " new=" << new_data.size());
    REQUIRE(bsdiff_create(old_data.data(), old_data.size(), new_data.data(),
                          new_data.size(), patch, err));
    REQUIRE(bsdiff_apply(old_data.data(), old_data.size(), patch.data(),
                         patch.size(), result, err));
    CHECK(result == new_data);
  };

  round_trip({}, bytes("hello fresh file"));
  round_trip(bytes("same"), bytes("same"));
  round_trip(bytes(""), bytes(""));
  round_trip(bytes("the quick brown fox"), bytes("the quick red fox!"));
  std::vector<uint8_t> bin(512);
  for (size_t i = 0; i < bin.size(); ++i) bin[i] = static_cast<uint8_t>(i % 251);
  std::vector<uint8_t> bin2 = bin;
  bin2[0] ^= 0xFF;
  bin2[255] ^= 0x01;
  bin2[511] ^= 0x80;
  bin2.insert(bin2.end(), {1, 2, 3, 0, 4});
  round_trip(bin, bin2);

  std::vector<uint8_t> big(256 * 1024, 'a');
  std::vector<uint8_t> big2 = big;
  big2[1000] = 'b';
  big2[200000] = 'c';
  big2.resize(big2.size() + 500, 'z');
  round_trip(big, big2);
}

TEST_CASE("bsdiff_apply rejects garbage", "[gmmpack]") {
  std::vector<uint8_t> oldv = bytes("base content here");
  std::vector<uint8_t> newv = bytes("base content THERE!");
  std::vector<uint8_t> patch, result;
  std::string err;
  REQUIRE(bsdiff_create(oldv.data(), oldv.size(), newv.data(), newv.size(),
                        patch, err));

  CHECK_FALSE(bsdiff_apply(oldv.data(), oldv.size(),
                           reinterpret_cast<const uint8_t*>("garbage"), 7,
                           result, err));
  CHECK_FALSE(bsdiff_apply(oldv.data(), oldv.size(), patch.data(),
                           patch.size() / 2, result, err));
  // Patch built for other content of a different length must fail loudly.
  std::vector<uint8_t> short_base = bytes("x");
  CHECK_FALSE(bsdiff_apply(short_base.data(), short_base.size(), patch.data(),
                           patch.size(), result, err));
  // Flipped magic fails loudly instead of mis-decoding.
  std::vector<uint8_t> bad = patch;
  bad[0] = 'X';
  CHECK_FALSE(bsdiff_apply(oldv.data(), oldv.size(), bad.data(), bad.size(),
                           result, err));
}

TEST_CASE("parse_patch_json accepts valid, rejects bad", "[gmmpack]") {
  BinaryPatch p;
  std::string err;
  auto oldv = bytes("v1"), newv = bytes("v2-patched");

  CHECK(parse_patch_json(make_json("skyui", oldv, newv, "SkyUI.esp"),
                         "skyui.json", p, err));
  CHECK(p.mod_id == "skyui");
  CHECK_FALSE(p.sequence.has_value());
  CHECK(p.target_path == "SkyUI.esp");

  CHECK(parse_patch_json(make_json("awm", oldv, newv, "A.esp", 2),
                         "awm-2.json", p, err));
  CHECK(p.sequence == 2);

  CHECK_FALSE(parse_patch_json(make_json("awm", oldv, newv, "A.esp", 2),
                               "other-2.json", p, err));  // modId mismatch
  CHECK_FALSE(parse_patch_json(make_json("awm", oldv, newv, "A.esp", 2),
                               "awm-3.json", p, err));  // sequence mismatch
  CHECK_FALSE(parse_patch_json(make_json("awm", oldv, newv, "A.esp", 2),
                               "awm.json", p, err));  // seq w/o suffix
  CHECK_FALSE(parse_patch_json("not json", "skyui.json", p, err));
  CHECK_FALSE(parse_patch_json("{\"modId\":\"x\"}", "x.json", p, err));
  std::string bad_algo = make_json("skyui", oldv, newv, "S.esp");
  bad_algo.replace(bad_algo.find("bsdiff"), 6, "xdiff");
  CHECK_FALSE(parse_patch_json(bad_algo, "skyui.json", p, err));
  std::string bad_b64 = make_json("skyui", oldv, newv, "S.esp");
  bad_b64.replace(bad_b64.find("payloadBase64\":\"") + 16, 4, "!!!!");
  CHECK_FALSE(parse_patch_json(bad_b64, "skyui.json", p, err));
}

TEST_CASE("group_patches orders chains, rejects gaps", "[gmmpack]") {
  auto oldv = bytes("v1"), mid = bytes("v2"), fin = bytes("v3");
  BinaryPatch a, b;
  std::string err;
  REQUIRE(parse_patch_json(make_json("m", mid, fin, "F.esp", 2), "m-2.json", a,
                           err));
  REQUIRE(parse_patch_json(make_json("m", oldv, mid, "F.esp", 1), "m-1.json", b,
                           err));
  std::vector<BinaryPatch> in{a, b};  // deliberately out of order
  std::vector<PatchChain> chains;
  REQUIRE(group_patches(std::move(in), chains, err));
  REQUIRE(chains.size() == 1);
  CHECK(chains[0].steps[0].sequence == 1);
  CHECK(chains[0].steps[1].sequence == 2);

  // Gap: only step 2.
  std::vector<PatchChain> out;
  std::vector<BinaryPatch> gap{a};
  CHECK_FALSE(group_patches(std::move(gap), out, err));

  // Mixed sequenced + unsequenced for one target.
  BinaryPatch single;
  REQUIRE(parse_patch_json(make_json("m", oldv, mid, "F.esp"), "m.json", single,
                           err));
  std::vector<BinaryPatch> mixed{a, single};
  CHECK_FALSE(group_patches(std::move(mixed), out, err));
}

TEST_CASE("patch plan + apply end to end", "[gmmpack]") {
  fs::path tmp = fs::temp_directory_path() / "gmm_patch_test";
  fs::remove_all(tmp);
  fs::path mod = tmp / "awesome-mod";
  auto oldv = bytes("plugin data v1...."), mid = bytes("plugin data v2...."),
       fin = bytes("plugin data v3!!!");
  write_file(mod / "AwesomeMod.esp", str(oldv));

  BinaryPatch s1, s2;
  std::string err;
  REQUIRE(parse_patch_json(make_json("awesome-mod", oldv, mid, "AwesomeMod.esp",
                                     1),
                           "awesome-mod-1.json", s1, err));
  REQUIRE(parse_patch_json(make_json("awesome-mod", mid, fin, "AwesomeMod.esp",
                                     2),
                           "awesome-mod-2.json", s2, err));
  std::vector<BinaryPatch> in{s2, s1};
  std::vector<PatchChain> chains;
  REQUIRE(group_patches(std::move(in), chains, err));
  PatchPlan plan = build_plan(std::move(chains));
  REQUIRE(plan.affected_mods == std::vector<std::string>{"awesome-mod"});

  std::unordered_map<std::string, fs::path> dirs{{"awesome-mod", mod}};
  REQUIRE(apply_plan(plan, dirs, err));
  CHECK(read_file(mod / "AwesomeMod.esp") == str(fin));

  // Base changed upstream: refuse, file untouched.
  write_file(mod / "AwesomeMod.esp", "tampered by user update");
  CHECK_FALSE(apply_plan(plan, dirs, err));
  CHECK(read_file(mod / "AwesomeMod.esp") == "tampered by user update");

  // Traversal target rejected.
  PatchChain evil;
  evil.mod_id = "awesome-mod";
  evil.target_path = "../../evil.esp";
  evil.steps.push_back(s1);
  CHECK_FALSE(apply_chain(evil, mod, err));

  // Unknown mod rejected.
  PatchPlan bad_plan = plan;
  bad_plan.chains[0].mod_id = "ghost-mod";
  CHECK_FALSE(apply_plan(bad_plan, dirs, err));

  fs::remove_all(tmp);
}

TEST_CASE("empty plan applies trivially", "[gmmpack]") {
  PatchPlan plan = build_plan({});
  CHECK(plan.affected_mods.empty());
  std::string err;
  CHECK(apply_plan(plan, {}, err));
}
