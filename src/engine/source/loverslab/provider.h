#pragma once

#include "engine/source/interface.h"

#include <string>

namespace engine::Source::LoversLab {

// Downloads LoversLab files. LoversLab has no public API: downloads are
// authorized by the user's browser session, so this provider sends the stored
// session cookie (Auth) as the Cookie header while fetching the
// pasted ?do=download&r=<revision>&confirm=1&t=1&csrfKey=<key> link. The
// archive name is resolved from the final response's Content-Disposition (or
// the effective URL path) so .7z/.rar/.zip archives keep their real extension.
class Provider : public Interface {
public:
    std::string source_type() const override { return "loverslab"; }
    bool fetch(const ::engine::Mod& mod, ::engine::PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;
    SourceDownloadInfo resolve_download_info(const ::engine::Mod& mod) const override;
    std::string display_name() const override;

    // URL helpers (pure, unit-tested, reused by the future browser-extension
    // flow that will hand GMM a bare file-page link).
    static bool is_loverslab_url(const std::string& url);
    // "https://www.loverslab.com/files/file/12345-slug/?do=download..." -> "12345"
    static std::string extract_file_id(const std::string& url);
    // "https://www.loverslab.com/files/file/12345-slug/?do=download&..." ->
    // "https://www.loverslab.com/files/file/12345-slug/" (query/fragment
    // stripped). A bare page link is returned unchanged.
    static std::string mod_page_url(const std::string& url);
};

} // namespace engine::Source::LoversLab
