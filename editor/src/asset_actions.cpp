// Aero Engine — the orphan-sidecar delete action (task 3.1.3). The FIFTH editor/src TU to include
// <filesystem>, and it holds EXACTLY ONE std::filesystem::remove call (§V6 greps both facts). NEVER
// LOGS (INV-V8/INV-A3's posture extended here): every exit is a RESULT, never a printed line.
#include <aero/editor/asset_actions.hpp>
#include <aero/editor/asset_cache.hpp>  // task E.4.3 -- ASSET_CACHE_DIR_NAME, the trash's parent
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/text_file.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace engine::editor {

namespace {

// project_files.cpp:29-33's precedent, copied TU-locally: construct from UTF-8 BYTES so non-ASCII
// names resolve correctly on Windows, where path's native encoding is UTF-16 and the narrow-char
// constructor assumes the active code page (NOT UTF-8).
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

// code-review finding 5: a pure STRING check, deliberately never std::filesystem::path::is_absolute()
// -- project.hpp:90-93's own A19 rationale, copied here: on Windows is_absolute("/shared") is FALSE
// (no root name), so a POSIX-rooted value would sail through a check meant to require ONE. Non-empty,
// and starts with '/' (POSIX, and the forward-slash form this tree's own roots already use) or a
// two-character ASCII drive-letter prefix ("C:", whatever follows). Deliberately NOT full UNC/registry
// validation -- this is a REFUSAL gate for an obviously-wrong root (an empty string, or a bare relative
// path), not a general path-legality checker; validateOrphanPath above already owns the relative half.
bool looksLikeAnAbsoluteRoot(std::string_view root) noexcept {
    if (root.empty()) {
        return false;
    }
    if (root.front() == '/') {
        return true;
    }
    if (root.size() >= 2) {
        const char first = root[0];
        const bool isAsciiLetter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
        if (isAsciiLetter && root[1] == ':') {
            return true;
        }
    }
    return false;
}

// task E.4.3: the case-only carve-out, shared by planner rungs 9/10 and (from the next commit)
// executor steps 4/6b so the two cannot disagree. ASCII-only case folding, deliberately: a
// Unicode-aware fold would need ICU, which this project does not link and will not, and the rule is
// about a volume's case-insensitivity for the ASCII names the editor can create at all
// (validateAssetName already refuses every byte the rule would be ambiguous for).
[[nodiscard]] bool asciiCaseEqual(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    const auto fold = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; };
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (fold(a[i]) != fold(b[i])) {
            return false;
        }
    }
    return true;
}

// task E.4.3: four digits, no locale, no <iomanip>, no std::format -- which is what makes the trash
// path assertable IDENTICALLY on three OSes (formatFileSize's own reasoning, one file over).
[[nodiscard]] std::string zeroPad4(std::uint32_t value) {
    std::string digits = std::to_string(value);
    while (digits.size() < 4) {
        digits.insert(digits.begin(), '0');
    }
    return digits;
}

// task E.4.3: DATA, never code, and never a platform branch. CON/PRN/AUX/NUL plus COM1-9 and LPT1-9
// -- the whole Windows reserved-device roster, compared case-insensitively against the name's STEM
// (everything before the FIRST '.'), because "CON.png" and "con.tar.gz" are reserved too.
constexpr std::array<std::string_view, 22> RESERVED_DEVICE_NAMES{
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

// task E.4.3: the seven bytes Windows refuses in a filename. '/' and '\\' are NOT here -- they are
// HasSeparator's, which is a different refusal with a different message.
constexpr std::string_view RESERVED_NAME_CHARACTERS = R"(*?"<>|:)";

// task E.4.3: resolve an AssetOpPath against the right root. The WHOLE reason AssetPathBase exists:
// a Delete's destination is under the PROJECT root and everything else is under the ASSETS root, and
// a single "relative" string gives the caller no way to tell which to prepend.
[[nodiscard]] std::string absoluteFor(const AssetOpPath& p, std::string_view projectRootUtf8,
                                      std::string_view assetsRootUtf8) {
    const std::string_view root = p.base == AssetPathBase::ProjectRoot ? projectRootUtf8 : assetsRootUtf8;
    return p.relative.empty() ? std::string(root) : std::string(root) + "/" + p.relative;
}

// task E.4.3: the executor's live free-name check, with the SAME case-only carve-out the planner's
// rungs 9 and 10 apply -- shared through asciiCaseEqual so the two cannot disagree. On a
// case-INSENSITIVE volume fileExists("a/Wood.png") is TRUE when only a/wood.png exists, so a naive
// step 4 would refuse every case-only rename; the carve-out is what makes wood.png -> Wood.png work
// there, and it is keyed on the step's OWN source leaf rather than on a general exemption.
//
// THE CARVE-OUT IS GATED ON THE DESTINATION ACTUALLY BEING THE SOURCE, AND THE LEXICAL CONDITION
// ALONE IS NOT ENOUGH (code-review G5 -- this shipped once as silent DATA LOSS). "A case-only rename
// within one directory can only collide with the source itself" is true on a case-INSENSITIVE volume
// and FALSE on a case-sensitive one, where a/wood.png and a/Wood.png are two genuinely different
// files. Without the equivalence test, on Linux: rung 9 passes because the listing holds no
// Wood.png; something external creates a/Wood.png between that listDirectory and this call -- which
// is precisely the window step 4 exists to close, and precisely seed S9 / validation row 6; step 4
// answers "not blocked"; rename OVERWRITES it; performed == true and the user's file is gone.
//
// std::filesystem::equivalent is true iff both paths resolve to the SAME FILE, so it is true exactly
// on the volumes where the carve-out is legitimate and false on the ones where it is not. The
// error_code overload never throws and answers false on any error, which fails SAFE (a refusal).
// Both callers -- step 4 and step 6b -- reach this having already established that both paths exist,
// so an error here is a genuine filesystem problem and refusing is right.
//
// THE CASE-SENSITIVE ARM IS UNOBSERVABLE ON macOS AND WINDOWS, whose default volumes are
// case-insensitive: there the equivalence is true and the carve-out applies exactly as before. Only
// the Linux lane executes the refusal this fix adds.
[[nodiscard]] bool destinationBlocked(const std::string& fromAbs, const std::string& toAbs, std::string_view fromLeaf,
                                      std::string_view toLeaf) {
    if (!fileExists(toAbs)) {
        return false;
    }
    if (fromAbs == toAbs) {
        return true;  // not a rename at all
    }
    if (!asciiCaseEqual(fromLeaf, toLeaf) || parentOf(fromAbs) != parentOf(toAbs)) {
        return true;  // a different name entirely, or a different directory: a real collision
    }
    // A case-only rename within one directory. It is only NOT a collision when the destination IS
    // the source -- which is what a case-insensitive volume makes true and a case-sensitive one does
    // not.
    std::error_code ec;
    const bool sameFile = std::filesystem::equivalent(pathFromUtf8(fromAbs), pathFromUtf8(toAbs), ec);
    return ec ? true : !sameFile;
}

// task E.4.3: rung 6's segment test, spelled once. `dest` is INSIDE `src` iff it IS src or it begins
// with src followed by a separator. NEVER a raw prefix: "tex" must not read as a prefix of
// "textures/a".
[[nodiscard]] bool isInsideOrEqual(std::string_view outer, std::string_view inner) noexcept {
    if (inner == outer) {
        return true;
    }
    return inner.size() > outer.size() && inner.compare(0, outer.size(), outer) == 0 && inner[outer.size()] == '/';
}

}  // namespace

// task E.4.3: validateOrphanPath's path-shape half, PROMOTED verbatim -- same checks, same order,
// same pointer+length arithmetic. The file operations arriving in this task decide "is this a safe
// assets-relative path" through this one function rather than four copies of it.
AssetOpRefusal validateRelativeAssetPath(std::string_view relativePath) noexcept {
    if (relativePath.empty()) {
        return AssetOpRefusal::BadSourcePath;
    }
    for (const char c : relativePath) {
        if (c == '\\') {
            return AssetOpRefusal::BadSourcePath;  // a Windows separator must never reach a
                                                   // relative key (2.2.4's paths are '/'-only)
        }
    }
    if (relativePath.front() == '/') {
        return AssetOpRefusal::BadSourcePath;  // an absolute POSIX path
    }
    if (relativePath.size() >= 2) {
        const char first = relativePath[0];
        const bool isAsciiLetter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
        if (isAsciiLetter && relativePath[1] == ':') {
            return AssetOpRefusal::BadSourcePath;  // a rooted Windows drive letter ("C:...")
        }
    }
    // ANY ".." SEGMENT, not merely the substring -- "..foo" and "foo.." are legal leaf names.
    // Pointer+length construction throughout (never substr): this function is noexcept, and substr
    // may throw std::out_of_range -- bugprone-exception-escape (asset_view.cpp's rawExtensionOf
    // precedent, task 3.1.3's own earlier fix for the identical trap).
    std::size_t start = 0;
    while (start <= relativePath.size()) {
        const std::size_t slash = relativePath.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? relativePath.size() : slash;
        const std::string_view segment(relativePath.data() + start, end - start);
        if (segment == "..") {
            return AssetOpRefusal::BadSourcePath;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return AssetOpRefusal::None;
}

OrphanDeleteRefusal validateOrphanPath(std::string_view relativeMetaPath) noexcept {
    const std::string_view leaf = leafOf(relativeMetaPath);
    if (!isMetaFileName(leaf)) {
        return OrphanDeleteRefusal::NotAMetaName;  // covers an empty path too (leafOf("") == "")
    }
    // task E.4.3: the path-shape half is now validateRelativeAssetPath, shared with the file
    // operations. The ORDER is unchanged -- the leaf test still runs FIRST, so an empty path and a
    // non-sidecar name still report NotAMetaName and not EscapesRoot. AA1-AA24 pass UNEDITED, and
    // that is the proof this promotion was behaviour-free.
    return validateRelativeAssetPath(relativeMetaPath) == AssetOpRefusal::None ? OrphanDeleteRefusal::None
                                                                               : OrphanDeleteRefusal::EscapesRoot;
}

AssetNameRefusal validateAssetName(std::string_view leaf) noexcept {
    // Checked in ENUMERATOR ORDER, so the refusal a caller sees is the FIRST rule the name broke --
    // deterministic, and what the AA battery asserts.
    if (leaf.empty()) {
        return AssetNameRefusal::Empty;
    }
    if (leaf.size() > MAX_ASSET_NAME_BYTES) {
        return AssetNameRefusal::TooLong;
    }
    for (const char c : leaf) {
        if (c == '/' || c == '\\') {
            return AssetNameRefusal::HasSeparator;
        }
    }
    if (leaf == "." || leaf == "..") {
        return AssetNameRefusal::DotOrDotDot;
    }
    if (leaf.front() == '.') {
        return AssetNameRefusal::Hidden;
    }
    for (const char c : leaf) {
        // UNSIGNED, deliberately. A validator walking `char` as SIGNED reads a UTF-8 lead byte such
        // as 0xc3 as NEGATIVE, and a naive `c < 0x20` test then fires on every non-ASCII name.
        if (static_cast<unsigned char>(c) < 0x20U) {
            return AssetNameRefusal::ControlCharacter;
        }
    }
    for (const char c : leaf) {
        if (RESERVED_NAME_CHARACTERS.find(c) != std::string_view::npos) {
            return AssetNameRefusal::ReservedCharacter;
        }
    }
    if (leaf.back() == '.' || leaf.back() == ' ') {
        return AssetNameRefusal::TrailingDotOrSpace;
    }
    // The STEM -- everything before the FIRST '.', or the whole leaf when it has none. "CON.png" and
    // "con.tar.gz" are both reserved; "CONS", "COM0" and "COM10" are NOT, which is what proves the
    // matcher is an equality over the roster and never a prefix test.
    // Pointer + length, NEVER substr: this function is noexcept and substr may throw
    // std::out_of_range, which bugprone-exception-escape rejects outright (validateRelativeAssetPath
    // above carries the identical rule for the identical reason).
    const std::size_t dot = leaf.find('.');
    const std::string_view stem(leaf.data(), dot == std::string_view::npos ? leaf.size() : dot);
    for (const std::string_view reserved : RESERVED_DEVICE_NAMES) {
        if (asciiCaseEqual(stem, reserved)) {
            return AssetNameRefusal::ReservedDeviceName;
        }
    }
    if (isMetaFileName(leaf)) {
        return AssetNameRefusal::MetaSuffix;
    }
    if (leaf.size() > ATOMIC_TEMP_SUFFIX.size() &&
        leaf.compare(leaf.size() - ATOMIC_TEMP_SUFFIX.size(), ATOMIC_TEMP_SUFFIX.size(), ATOMIC_TEMP_SUFFIX) == 0) {
        return AssetNameRefusal::TempSuffix;
    }
    return AssetNameRefusal::None;
}

AssetOpRefusal assetOpPathLadder(AssetOpKind kind, std::string_view sourceRelative,
                                 std::string_view destinationDirRelative) noexcept {
    // Rung 1: the assets root is the PROJECT's, not the browser's. CreateFolder is the one kind for
    // which "" is a legal source -- there it names the PARENT directory the folder is created in.
    if (sourceRelative.empty() && kind != AssetOpKind::CreateFolder) {
        return AssetOpRefusal::SourceIsRoot;
    }
    // Rung 2, skipped for CreateFolder's legal "" parent.
    if (!(sourceRelative.empty() && kind == AssetOpKind::CreateFolder) &&
        validateRelativeAssetPath(sourceRelative) != AssetOpRefusal::None) {
        return AssetOpRefusal::BadSourcePath;
    }
    if (kind != AssetOpKind::Move) {
        return AssetOpRefusal::None;  // rungs 3, 5 and 6 are Move's alone
    }
    // Rung 3: "" is the assets root and is a LEGAL destination.
    if (!destinationDirRelative.empty() && validateRelativeAssetPath(destinationDirRelative) != AssetOpRefusal::None) {
        return AssetOpRefusal::BadDestination;
    }
    // Rung 5: already where it is asked to go. A no-op, not an error.
    if (destinationDirRelative == parentOf(sourceRelative)) {
        return AssetOpRefusal::AlreadyThere;
    }
    // Rung 6: SEGMENT-WISE, never a raw prefix.
    if (isInsideOrEqual(sourceRelative, destinationDirRelative)) {
        return AssetOpRefusal::DestinationInsideSource;
    }
    return AssetOpRefusal::None;
}

AssetOpRefusal classifyAssetMove(std::string_view sourceRelative, std::string_view destinationDirRelative) noexcept {
    return assetOpPathLadder(AssetOpKind::Move, sourceRelative, destinationDirRelative);
}

namespace {

// task E.4.3: does `entries` already hold `leaf`? The case-only carve-out is the CALLER's -- a
// Rename may collide with the SOURCE's own entry on a case-insensitive volume and that is legal,
// while a Move into a different directory has no such argument.
[[nodiscard]] bool listingHolds(const DirectoryListing& listing, std::string_view leaf,
                                std::string_view caseOnlyExemptLeaf) noexcept {
    for (const FileEntry& entry : listing.entries) {
        if (entry.name == leaf) {
            return true;  // an exact byte match is a collision whatever the carve-out says
        }
        if (!asciiCaseEqual(entry.name, leaf)) {
            continue;  // not a collision on any volume
        }
        // A case-only match. On a case-INSENSITIVE volume it is the same name, so it IS a collision
        // -- unless it is the source's own entry, which is exactly what a case-only RENAME is moving
        // out of the way. Refusing there would make wood.png -> Wood.png impossible on macOS and
        // Windows while working on Linux.
        if (entry.name == caseOnlyExemptLeaf) {
            continue;
        }
        return true;
    }
    return false;
}

}  // namespace

namespace {

// task E.4.3: the human sentence for a PLANNER refusal. The executor composes its own (an
// error_code's message, or a path pair), so this covers the rungs alone. Without it every planner
// refusal reached the log as an empty clause -- measured through I221(a), whose WARN read
// "refused to move 'a/b.png' --  (NameTaken)".
[[nodiscard]] std::string plannerRefusalMessage(AssetOpRefusal refusal) {
    switch (refusal) {
        case AssetOpRefusal::None:
            return {};
        case AssetOpRefusal::SourceIsRoot:
            return "the assets root itself cannot be renamed, moved or deleted";
        case AssetOpRefusal::BadSourcePath:
            return "that is not a safe assets-relative path";
        case AssetOpRefusal::BadDestination:
            return "the destination is not a safe assets-relative path";
        case AssetOpRefusal::BadName:
            return "that name cannot be used";  // nameRefusal carries WHICH rule
        case AssetOpRefusal::AlreadyThere:
            return "it is already in that folder";
        case AssetOpRefusal::DestinationInsideSource:
            return "a folder cannot be moved into itself or into one of its own subfolders";
        case AssetOpRefusal::DestinationMissing:
            return "the destination folder does not exist";
        case AssetOpRefusal::ListingIncomplete:
            return "the destination folder could not be read in full";
        case AssetOpRefusal::NameTaken:
            return "something with that name is already there";
        case AssetOpRefusal::SidecarBlocked:
            return "its .meta sidecar's destination name is already taken";
        // The executor's own outcomes: it composes each message itself, from an error_code or a path
        // pair, so none of them is ever produced by the planner.
        case AssetOpRefusal::NoProject:
        case AssetOpRefusal::SourceMissing:
        case AssetOpRefusal::TrashUnavailable:
        case AssetOpRefusal::RenameFailed:
        case AssetOpRefusal::SidecarRenameFailed:
        case AssetOpRefusal::RollbackFailed:
            return {};
    }
    return {};  // unreachable; enumerated so a new refusal is a -Wswitch warning
}

}  // namespace

AssetOpPlan planAssetOp(AssetOpKind kind, const AssetOpInputs& inputs, const DirectoryListing& destinationListing) {
    AssetOpPlan plan;
    // Every `return plan` below runs through this, so a rung cannot ship a refusal with no sentence.
    const auto refuse = [&plan](AssetOpRefusal refusal) -> AssetOpPlan& {
        plan.refusal = refusal;
        plan.message = plannerRefusalMessage(refusal);
        return plan;
    };

    // Rungs 1, 2, 3, 5 and 6 -- the SHARED ladder, called rather than restated, so classifyAssetMove
    // and this function cannot disagree about DestinationInsideSource.
    if (const AssetOpRefusal ladder = assetOpPathLadder(kind, inputs.sourceRelative, inputs.destinationDirRelative);
        ladder != AssetOpRefusal::None) {
        return refuse(ladder);
    }

    // Rung 4: the typed leaf, for the two kinds that carry one.
    if (kind == AssetOpKind::Rename || kind == AssetOpKind::CreateFolder) {
        plan.nameRefusal = validateAssetName(inputs.newLeaf);
        if (plan.nameRefusal != AssetNameRefusal::None) {
            return refuse(AssetOpRefusal::BadName);
        }
    }

    // Rung 7 sits ABOVE rung 8 and the order is load-bearing: listingIsComplete is false for a
    // non-Ok status too, so with the two swapped a destination that does not EXIST would be reported
    // as "the folder could not be read in full" -- true, and useless.
    if (kind == AssetOpKind::Move &&
        (destinationListing.status == ScanStatus::Missing || destinationListing.status == ScanStatus::NotADirectory)) {
        return refuse(AssetOpRefusal::DestinationMissing);
    }
    // Rung 8: a PREFIX cannot prove a name is free. Skipped for Delete, whose trash sequence
    // directory is fresh by construction and whose caller therefore passes DirectoryListing{}.
    if (kind != AssetOpKind::Delete && !listingIsComplete(destinationListing)) {
        return refuse(AssetOpRefusal::ListingIncomplete);
    }

    // ---- step composition. The switch has NO `default:` so a fifth kind is a -Wswitch error. ----
    const std::string_view sourceLeaf = leafOf(inputs.sourceRelative);
    switch (kind) {
        case AssetOpKind::CreateFolder: {
            const std::string target = joinRelative(inputs.sourceRelative, inputs.newLeaf);
            if (listingHolds(destinationListing, inputs.newLeaf, {})) {
                return refuse(AssetOpRefusal::NameTaken);
            }
            plan.directoryToCreate = AssetOpPath{AssetPathBase::AssetsRoot, target};
            plan.stepCount = 0;
            plan.resultingRelativePath = target;
            break;
        }
        case AssetOpKind::Rename: {
            const std::string parent = parentOf(inputs.sourceRelative);
            const std::string target = joinRelative(parent, inputs.newLeaf);
            // Rung 9, with the case-only carve-out: a rename whose new leaf ASCII-case-folds equal
            // to the old one and differs byte-wise is PERMITTED.
            if (listingHolds(destinationListing, inputs.newLeaf, sourceLeaf)) {
                return refuse(AssetOpRefusal::NameTaken);
            }
            plan.steps[0] = AssetOpStep{AssetOpPath{AssetPathBase::AssetsRoot, std::string(inputs.sourceRelative)},
                                        AssetOpPath{AssetPathBase::AssetsRoot, target}};
            plan.stepCount = 1;
            if (!inputs.sourceIsDirectory) {
                // Rung 10: the sidecar's destination name, refused BEFORE any step runs -- the one
                // point at which refusing a half-move is still free.
                const std::string sidecarLeaf = metaFileNameFor(inputs.newLeaf);
                if (listingHolds(destinationListing, sidecarLeaf, metaFileNameFor(sourceLeaf))) {
                    plan.stepCount = 0;
                    plan.steps = {};
                    return refuse(AssetOpRefusal::SidecarBlocked);
                }
                plan.steps[1] = AssetOpStep{
                    AssetOpPath{AssetPathBase::AssetsRoot, joinRelative(parent, metaFileNameFor(sourceLeaf))},
                    AssetOpPath{AssetPathBase::AssetsRoot, joinRelative(parent, sidecarLeaf)}};
                plan.stepCount = 2;
            }
            plan.resultingRelativePath = target;
            break;
        }
        case AssetOpKind::Move: {
            const std::string target = joinRelative(inputs.destinationDirRelative, sourceLeaf);
            // No carve-out here: a move to a DIFFERENT directory has no "the entry it collides with
            // is the source itself" argument.
            if (listingHolds(destinationListing, sourceLeaf, {})) {
                return refuse(AssetOpRefusal::NameTaken);
            }
            plan.steps[0] = AssetOpStep{AssetOpPath{AssetPathBase::AssetsRoot, std::string(inputs.sourceRelative)},
                                        AssetOpPath{AssetPathBase::AssetsRoot, target}};
            plan.stepCount = 1;
            if (!inputs.sourceIsDirectory) {
                const std::string sidecarLeaf = metaFileNameFor(sourceLeaf);
                if (listingHolds(destinationListing, sidecarLeaf, {})) {
                    plan.stepCount = 0;
                    plan.steps = {};
                    return refuse(AssetOpRefusal::SidecarBlocked);
                }
                plan.steps[1] = AssetOpStep{
                    AssetOpPath{AssetPathBase::AssetsRoot, joinRelative(parentOf(inputs.sourceRelative), sidecarLeaf)},
                    AssetOpPath{AssetPathBase::AssetsRoot, joinRelative(inputs.destinationDirRelative, sidecarLeaf)}};
                plan.stepCount = 2;
            }
            plan.resultingRelativePath = target;
            break;
        }
        case AssetOpKind::Delete: {
            // The MIXED-BASE case, and the one a single-base design gets silently wrong: the source
            // is under the ASSETS root and the destination under the PROJECT root.
            const std::string trashTarget = trashRelativePathFor(inputs.trashSequence, inputs.sourceRelative);
            plan.steps[0] = AssetOpStep{AssetOpPath{AssetPathBase::AssetsRoot, std::string(inputs.sourceRelative)},
                                        AssetOpPath{AssetPathBase::ProjectRoot, trashTarget}};
            plan.stepCount = 1;
            plan.directoryToCreate = AssetOpPath{AssetPathBase::ProjectRoot, parentOf(trashTarget)};
            if (!inputs.sourceIsDirectory) {
                const std::string sidecarLeaf = metaFileNameFor(sourceLeaf);
                const std::string sidecarRel = joinRelative(parentOf(inputs.sourceRelative), sidecarLeaf);
                plan.steps[1] = AssetOpStep{
                    AssetOpPath{AssetPathBase::AssetsRoot, sidecarRel},
                    AssetOpPath{AssetPathBase::ProjectRoot, trashRelativePathFor(inputs.trashSequence, sidecarRel)}};
                plan.stepCount = 2;
            }
            // ALWAYS "" -- the file is no longer in the assets tree, so there is nothing to select.
            plan.resultingRelativePath.clear();
            break;
        }
    }
    return plan;
}

std::size_t countRecordsUnder(std::span<const AssetRecord> records, std::string_view folderRelative) noexcept {
    if (folderRelative.empty()) {
        return records.size();  // the assets root holds everything
    }
    // SEGMENT-WISE: "tex" must not count "textures/a.png". records() is sorted byte-lexicographically
    // by relativePath, so this is a lower_bound on the folder NAME plus a walk while that prefix
    // holds. ALLOCATION-FREE on purpose: this function is noexcept, and composing a
    // `folderRelative + '/'` key here could throw std::bad_alloc, which bugprone-exception-escape
    // rejects outright. Starting from the folder name itself is equivalent because every path
    // beginning with those bytes is contiguous in byte order, whatever byte follows them.
    const auto first = std::lower_bound(
        records.begin(), records.end(), folderRelative,
        [](const AssetRecord& record, std::string_view key) { return std::string_view(record.relativePath) < key; });
    std::size_t count = 0;
    for (auto it = first; it != records.end(); ++it) {
        const std::string_view path(it->relativePath);
        if (path.size() < folderRelative.size() || path.compare(0, folderRelative.size(), folderRelative) != 0) {
            break;  // past every path that begins with this folder's name
        }
        // The SEGMENT test: "textures.png" begins with "textures" and is NOT inside it.
        if (path.size() > folderRelative.size() && path[folderRelative.size()] == '/') {
            ++count;
        }
    }
    return count;
}

std::string trashRelativePathFor(std::uint32_t sequence, std::string_view assetsRelativePath) {
    // Composed through joinRelative, never '+': an empty assetsRelativePath must not produce a
    // trailing separator, which is that helper's own stated rule.
    std::string path = joinRelative(std::string(ASSET_CACHE_DIR_NAME), ASSET_TRASH_DIR_NAME);
    path = joinRelative(path, zeroPad4(sequence));
    return joinRelative(path, assetsRelativePath);
}

AssetDeletePrompt assetDeletePromptFor(std::string_view relativePath, bool isDirectory, std::size_t indexedAssetCount) {
    AssetDeletePrompt prompt;
    // Every byte is ASCII and no sentence contains a '%'. `--` is an ASCII DOUBLE HYPHEN, never an en
    // dash: a UTF-8 dash would make the byte comparisons that pin these strings depend on the source
    // encoding.
    if (isDirectory) {
        prompt.title = "Delete folder \"" + std::string(leafOf(relativePath)) + "\"?";
        prompt.detail = "It holds " + std::to_string(indexedAssetCount) + " indexed asset";
        if (indexedAssetCount != 1) {
            prompt.detail += "s";
        }
        prompt.detail += ". Everything inside it moves to the project trash together.";
        prompt.footer = "The files are not erased -- they move to Library/Trash/ inside this project.";
    } else {
        prompt.title = "Delete \"" + std::string(relativePath) + "\"?";
        prompt.detail = "Its .meta sidecar moves with it, so its identity is preserved if you put it back.";
        prompt.footer = "The file is not erased -- it moves to Library/Trash/ inside this project.";
    }
    return prompt;
}

std::string assetNameRefusalMessage(AssetNameRefusal refusal) {
    // No `default:` -- a new enumerator is a -Wswitch error. None is "" so the modal draws no error
    // line at all when the name is legal.
    switch (refusal) {
        case AssetNameRefusal::None:
            return {};
        case AssetNameRefusal::Empty:
            return "Enter a name.";
        case AssetNameRefusal::TooLong:
            return "That name is longer than 255 bytes, which no supported filesystem accepts.";
        case AssetNameRefusal::HasSeparator:
            return "A name cannot contain a slash. Use drag-and-drop to move an asset instead.";
        case AssetNameRefusal::DotOrDotDot:
            // A custom raw delimiter: the default `)"` would terminate at the first `.")` in the
            // text itself, which silently truncates the sentence rather than failing to compile.
            return R"MSG("." and ".." are not names.)MSG";
        case AssetNameRefusal::Hidden:
            return "A name starting with a dot is hidden, and the asset browser would not show it.";
        case AssetNameRefusal::ControlCharacter:
            return "That name contains a control character.";
        case AssetNameRefusal::ReservedCharacter:
            return "A name cannot contain any of * ? \" < > | : -- Windows refuses them.";
        case AssetNameRefusal::TrailingDotOrSpace:
            return "A name cannot end in a dot or a space -- Windows strips those silently.";
        case AssetNameRefusal::ReservedDeviceName:
            return "That is a reserved device name on Windows, with or without an extension.";
        case AssetNameRefusal::MetaSuffix:
            return "A name cannot end in .meta -- that is what an asset's sidecar is called.";
        case AssetNameRefusal::TempSuffix:
            return "A name cannot end in .aero-tmp -- the asset browser skips those.";
    }
    return {};  // unreachable; enumerated so a new refusal is a -Wswitch warning, not silent
}

std::string_view assetNameRefusalLabel(AssetNameRefusal refusal) noexcept {
    switch (refusal) {
        case AssetNameRefusal::None:
            return "None";
        case AssetNameRefusal::Empty:
            return "Empty";
        case AssetNameRefusal::TooLong:
            return "TooLong";
        case AssetNameRefusal::HasSeparator:
            return "HasSeparator";
        case AssetNameRefusal::DotOrDotDot:
            return "DotOrDotDot";
        case AssetNameRefusal::Hidden:
            return "Hidden";
        case AssetNameRefusal::ControlCharacter:
            return "ControlCharacter";
        case AssetNameRefusal::ReservedCharacter:
            return "ReservedCharacter";
        case AssetNameRefusal::TrailingDotOrSpace:
            return "TrailingDotOrSpace";
        case AssetNameRefusal::ReservedDeviceName:
            return "ReservedDeviceName";
        case AssetNameRefusal::MetaSuffix:
            return "MetaSuffix";
        case AssetNameRefusal::TempSuffix:
            return "TempSuffix";
    }
    return "None";  // unreachable; enumerated so a new refusal is a -Wswitch warning, not silent
}

std::string_view assetOpRefusalLabel(AssetOpRefusal refusal) noexcept {
    // No `default:` -- a new enumerator is a -Wswitch error rather than a silent gap. That is the
    // whole reason neither refusal enum carries a Count sentinel.
    switch (refusal) {
        case AssetOpRefusal::None:
            return "None";
        case AssetOpRefusal::NoProject:
            return "NoProject";
        case AssetOpRefusal::BadSourcePath:
            return "BadSourcePath";
        case AssetOpRefusal::BadDestination:
            return "BadDestination";
        case AssetOpRefusal::SourceIsRoot:
            return "SourceIsRoot";
        case AssetOpRefusal::BadName:
            return "BadName";
        case AssetOpRefusal::SourceMissing:
            return "SourceMissing";
        case AssetOpRefusal::DestinationMissing:
            return "DestinationMissing";
        case AssetOpRefusal::DestinationInsideSource:
            return "DestinationInsideSource";
        case AssetOpRefusal::AlreadyThere:
            return "AlreadyThere";
        case AssetOpRefusal::ListingIncomplete:
            return "ListingIncomplete";
        case AssetOpRefusal::NameTaken:
            return "NameTaken";
        case AssetOpRefusal::SidecarBlocked:
            return "SidecarBlocked";
        case AssetOpRefusal::TrashUnavailable:
            return "TrashUnavailable";
        case AssetOpRefusal::RenameFailed:
            return "RenameFailed";
        case AssetOpRefusal::SidecarRenameFailed:
            return "SidecarRenameFailed";
        case AssetOpRefusal::RollbackFailed:
            return "RollbackFailed";
    }
    return "None";  // unreachable; enumerated so a new refusal is a -Wswitch warning, not silent
}

std::optional<std::uint32_t> allocateTrashSequence(std::string_view projectRootUtf8) {
    // fileExists ONLY: this function creates nothing and removes nothing, so it matches neither
    // FORBIDDEN_RE nor DELETE_RE. An INTEGER counter, never a timestamp -- currentFileTimeTicks() is
    // opaque file_time_type ticks and never a date (docs/09 §6.5), so a timestamped directory would
    // be non-deterministic and untestable, and would collide anyway at one-second resolution.
    for (std::uint32_t sequence = 1; sequence <= MAX_TRASH_SEQUENCE; ++sequence) {
        const std::string candidate =
            std::string(projectRootUtf8) + "/" + trashRelativePathFor(sequence, std::string_view{});
        if (!fileExists(candidate)) {
            return sequence;
        }
    }
    // Exhausted. The caller reports TrashUnavailable rather than reusing a directory: reuse would
    // put two deletes of the same path in one folder, where the second collides with the first.
    return std::nullopt;
}

AssetOpResult executeAssetOpPlan(const AssetOpPlan& plan, std::string_view projectRootUtf8,
                                 std::string_view assetsRootUtf8) {
    AssetOpResult result;

    // Step 0: refuse an empty or non-absolute root EXPLICITLY, before it is ever concatenated into a
    // path -- deleteOrphanMeta's own code-review finding 5, applied to BOTH roots this time.
    if (!looksLikeAnAbsoluteRoot(projectRootUtf8) || !looksLikeAnAbsoluteRoot(assetsRootUtf8)) {
        result.refusal = AssetOpRefusal::NoProject;
        result.message = "the project or assets root is empty or not absolute";
        return result;
    }

    // Step 1: a refused plan is returned VERBATIM -- both enumerators and the message -- and nothing
    // is touched.
    if (plan.refusal != AssetOpRefusal::None) {
        result.refusal = plan.refusal;
        result.nameRefusal = plan.nameRefusal;
        result.message = plan.message;
        return result;
    }

    // Step 2: the directory a CreateFolder makes, or the trash sequence directory a Delete needs.
    if (!plan.directoryToCreate.relative.empty()) {
        const std::string dirAbs = absoluteFor(plan.directoryToCreate, projectRootUtf8, assetsRootUtf8);
        if (const std::string error = ensureDirectory(dirAbs); !error.empty()) {
            // A Delete's directory is the trash's; anything else is the folder the user asked for.
            result.refusal = plan.directoryToCreate.base == AssetPathBase::ProjectRoot
                                 ? AssetOpRefusal::TrashUnavailable
                                 : AssetOpRefusal::RenameFailed;
            result.message = error;
            return result;
        }
    }
    if (plan.stepCount == 0) {
        // CreateFolder uses steps 0-2 only and renames NOTHING, which is what makes New Folder a
        // non-destructive operation entirely: ensureDirectory matches neither FORBIDDEN_RE nor
        // DELETE_RE.
        result.performed = true;
        result.resultingPath = plan.resultingRelativePath;
        return result;
    }

    const std::string fromAbs = absoluteFor(plan.steps[0].from, projectRootUtf8, assetsRootUtf8);
    const std::string toAbs = absoluteFor(plan.steps[0].to, projectRootUtf8, assetsRootUtf8);

    // Step 3: the source is re-verified from disk immediately before acting.
    if (!fileExists(fromAbs)) {
        result.refusal = AssetOpRefusal::SourceMissing;
        result.message = "the source no longer exists";
        return result;
    }
    // Step 3b: the destination's PARENT. "" is the root itself, which exists by step 0. This is the
    // rung that catches a folder deleted between the plan and the act.
    if (const std::string destParent = parentOf(plan.steps[0].to.relative); !destParent.empty()) {
        const AssetOpPath parentPath{plan.steps[0].to.base, destParent};
        if (!fileExists(absoluteFor(parentPath, projectRootUtf8, assetsRootUtf8))) {
            result.refusal = AssetOpRefusal::DestinationMissing;
            result.message = "the destination folder no longer exists";
            return result;
        }
    }
    // Step 4: a LIVE free-name check, ADDITIONAL to the planner's listing-based rung 9 and never a
    // replacement for it. The listing check produces the good message and catches the truncation
    // case; this one closes the window between reading the listing and acting. BOTH run.
    if (destinationBlocked(fromAbs, toAbs, leafOf(plan.steps[0].from.relative), leafOf(plan.steps[0].to.relative))) {
        result.refusal = AssetOpRefusal::NameTaken;
        result.message = "something with that name already exists";
        return result;
    }

    // Step 5: the ASSET or the FOLDER. The overwhelmingly common failure -- the name is taken, the
    // volume is read-only, the OS refuses -- happens HERE, where nothing has happened yet and the
    // refusal is free.
    std::error_code ec;
    std::filesystem::rename(pathFromUtf8(fromAbs), pathFromUtf8(toAbs), ec);
    if (ec) {
        result.refusal = AssetOpRefusal::RenameFailed;  // NOTHING HAPPENED
        result.message = ec.message();
        return result;
    }

    if (plan.stepCount == 2) {
        const std::string sidecarFromAbs = absoluteFor(plan.steps[1].from, projectRootUtf8, assetsRootUtf8);
        const std::string sidecarToAbs = absoluteFor(plan.steps[1].to, projectRootUtf8, assetsRootUtf8);
        // Step 6a: Missing is NOT an error (E21). There is no sidecar to carry, which is why an
        // unscanned file and an Invalid-state record both move cleanly.
        if (fileExists(sidecarFromAbs)) {
            // Step 6b/6c: either the sidecar's destination is occupied, or the OS refuses the rename.
            // Both roll step 5 back; a failure of the ROLLBACK itself is a distinct, loud outcome.
            const bool blocked = destinationBlocked(sidecarFromAbs, sidecarToAbs, leafOf(plan.steps[1].from.relative),
                                                    leafOf(plan.steps[1].to.relative));
            std::error_code sidecarEc;
            if (!blocked) {
                std::filesystem::rename(pathFromUtf8(sidecarFromAbs), pathFromUtf8(sidecarToAbs), sidecarEc);
            }
            if (blocked || sidecarEc) {
                std::error_code rollbackEc;
                std::filesystem::rename(pathFromUtf8(toAbs), pathFromUtf8(fromAbs), rollbackEc);
                if (rollbackEc) {
                    result.refusal = AssetOpRefusal::RollbackFailed;
                    result.torn = true;
                    // BOTH paths, so a manual recovery is possible from the log line alone.
                    result.message = "the asset moved to '" + plan.steps[0].to.relative +
                                     "' but its sidecar is still at '" + plan.steps[1].from.relative +
                                     "', and the rollback failed: " + rollbackEc.message();
                    return result;
                }
                result.refusal = blocked ? AssetOpRefusal::SidecarBlocked : AssetOpRefusal::SidecarRenameFailed;
                result.message =
                    blocked ? std::string("the sidecar's destination name is already taken") : sidecarEc.message();
                return result;
            }
        }
    }

    // Step 7.
    result.performed = true;
    result.resultingPath = plan.resultingRelativePath;
    return result;
}

OrphanDeleteResult deleteOrphanMeta(std::string_view assetsRootUtf8, std::string_view relativeMetaPath) {
    OrphanDeleteResult result;

    // 0: refuse an empty or non-absolute root EXPLICITLY, before it is ever concatenated into a path
    // (code-review finding 5). An empty root plus "/" plus a relative path resolves to the filesystem
    // ROOT ("" + "/" + "wood.png.meta" -> "/wood.png.meta") -- AssetDatabase::root() is genuinely empty
    // with no project open, and this function has no other guard against that. Reusing EscapesRoot: an
    // unresolvable root is conceptually the same failure as a path that escapes a real one.
    if (!looksLikeAnAbsoluteRoot(assetsRootUtf8)) {
        result.refusal = OrphanDeleteRefusal::EscapesRoot;
        result.message = "the assets root is empty or not absolute";
        return result;
    }

    // 1: refuse before any disk touch (E24).
    const OrphanDeleteRefusal pathRefusal = validateOrphanPath(relativeMetaPath);
    if (pathRefusal != OrphanDeleteRefusal::None) {
        result.refusal = pathRefusal;
        result.message = "not a safe sidecar path";
        return result;
    }

    const std::string absoluteMetaPath = std::string(assetsRootUtf8) + "/" + std::string(relativeMetaPath);

    // 2: Missing is NOT an error condition beyond the log line the caller writes (E21).
    if (!fileExists(absoluteMetaPath)) {
        result.refusal = OrphanDeleteRefusal::Missing;
        result.message = "the sidecar no longer exists";
        return result;
    }

    // 3: the action never deletes on a name alone -- a file literally called "notes.meta" that is
    // not a real sidecar survives (E23).
    const FileReadResult read = readTextFile(absoluteMetaPath);
    if (!read.text.has_value()) {
        result.refusal = OrphanDeleteRefusal::NotAMeta;
        result.message = "could not be read: " + read.error;
        return result;
    }
    const MetaParseResult parsed = parseMeta(*read.text);
    if (!parsed.guid.has_value()) {
        result.refusal = OrphanDeleteRefusal::NotAMeta;
        result.message = "does not parse as a .meta v1 sidecar";
        return result;
    }

    // 4: the check that stops a race from destroying a live identity (E22).
    const std::string_view metaLeaf = leafOf(relativeMetaPath);
    const std::string_view assetLeaf = assetNameForMeta(metaLeaf);
    const std::string assetRelPath = joinRelative(parentOf(relativeMetaPath), assetLeaf);
    const std::string absoluteAssetPath = std::string(assetsRootUtf8) + "/" + assetRelPath;
    if (fileExists(absoluteAssetPath)) {
        result.refusal = OrphanDeleteRefusal::AssetPresent;
        result.message = "the asset it describes exists again";
        return result;
    }

    // 5: the ONE sanctioned std::filesystem::remove in the editor.
    std::error_code ec;
    const bool removed = std::filesystem::remove(pathFromUtf8(absoluteMetaPath), ec);
    if (ec || !removed) {
        result.refusal = OrphanDeleteRefusal::RemoveFailed;
        result.message = ec ? ec.message() : std::string("the OS refused to remove the file");
        return result;
    }

    // 6.
    result.deleted = true;
    return result;
}

}  // namespace engine::editor
