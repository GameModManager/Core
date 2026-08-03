#pragma once

// Ported from FOMOD Plus (MIT, github.com/clearing/mo2-fomod-plus),
// share/xml/ModuleConfiguration.h/.cpp, FomodInfoFile, XmlHelper,
// XmlParseException and share/stringutil.h. The FOMOD Plus files are
// themselves ports of the Mod Organizer 2 FOMOD installer schema handling
// (the original installer is GPLv3 and was used as a spec reference only).
// See Core/NOTICE for the full attribution.
//
// This port is Qt-free: ModuleConfiguration::deserialize() takes a
// filesystem path instead of QString, and the FOMOD filename constants
// live in fomod_utils.h.

#include "engine/fomod/fomod_utils.h"

#include <filesystem>
#include <optional>
#include <ostream>
#include <pugixml.hpp>
#include <string>
#include <vector>

namespace engine {

class XmlParseException final : public std::runtime_error {
public:
    explicit XmlParseException(const std::string& message)
        : std::runtime_error(message) {}
};

class XmlDeserializable {
public:
    virtual ~XmlDeserializable() = default;
    virtual bool deserialize(pugi::xml_node& node) = 0;

protected:
    XmlDeserializable() = default;
};

enum GroupTypeEnum { SelectAny, SelectAll, SelectExactlyOne, SelectAtMostOne, SelectAtLeastOne };

enum class OperatorTypeEnum { AND, OR };

enum class OrderTypeEnum { Explicit, Ascending, Descending };

enum class FileDependencyTypeEnum { Missing, Inactive, Active, UNKNOWN_STATE };

template <typename T> class OrderedContents {
public:
    OrderTypeEnum order;

    OrderedContents() : order(OrderTypeEnum::Ascending) {}
    explicit OrderedContents(const OrderTypeEnum orderType) : order(orderType) {}

    template <typename Accessor> bool compare(const T& a, const T& b, Accessor accessor) const
    {
        switch (order) {
        case OrderTypeEnum::Ascending:
            return accessor(a) < accessor(b);
        case OrderTypeEnum::Descending:
            return accessor(a) > accessor(b);
        case OrderTypeEnum::Explicit:
        default:
            return false;  // No sorting for explicit order
        }
    }
};

enum class PluginTypeEnum { Recommended, Required, Optional, NotUsable, CouldBeUsable, UNKNOWN };

inline std::ostream& operator<<(std::ostream& os, const PluginTypeEnum& type)
{
    switch (type) {
    case PluginTypeEnum::Recommended:
        os << "Recommended";
        break;
    case PluginTypeEnum::Required:
        os << "Required";
        break;
    case PluginTypeEnum::Optional:
        os << "Optional";
        break;
    case PluginTypeEnum::NotUsable:
        os << "NotUsable";
        break;
    case PluginTypeEnum::CouldBeUsable:
        os << "CouldBeUsable";
        break;
    default:;
    }
    return os;
}

// Parse an order attribute with the same defaults FOMOD Plus uses:
// Ascending for plugin lists, Explicit for group/step lists.
inline OrderTypeEnum getOrderType(
    const std::string& orderType, OrderTypeEnum defaultOrder = OrderTypeEnum::Explicit)
{
    if (orderType == "Explicit")
        return OrderTypeEnum::Explicit;
    if (orderType == "Ascending")
        return OrderTypeEnum::Ascending;
    if (orderType == "Descending")
        return OrderTypeEnum::Descending;
    return defaultOrder;
}

class PluginType final : public XmlDeserializable {
public:
    PluginTypeEnum name = PluginTypeEnum::Optional;  // sane default

    bool deserialize(pugi::xml_node& node) override;
};

class FileDependency final : public XmlDeserializable {
public:
    std::string file;
    FileDependencyTypeEnum state = FileDependencyTypeEnum::UNKNOWN_STATE;

    bool deserialize(pugi::xml_node& node) override;
};

class FlagDependency final : public XmlDeserializable {
public:
    std::string flag;
    std::string value;

    bool deserialize(pugi::xml_node& node) override;
};

class GameDependency final : public XmlDeserializable {
public:
    std::string version;

    bool deserialize(pugi::xml_node& node) override;
};

class CompositeDependency final : public XmlDeserializable {
public:
    std::vector<FileDependency> fileDependencies;
    std::vector<FlagDependency> flagDependencies;
    std::vector<GameDependency> gameDependencies;
    std::vector<CompositeDependency> nestedDependencies;
    OperatorTypeEnum operatorType = OperatorTypeEnum::AND;  // safest default.

    bool deserialize(pugi::xml_node& node) override;
};

class DependencyPattern final : public XmlDeserializable {
public:
    CompositeDependency dependencies;
    PluginTypeEnum type;

    bool deserialize(pugi::xml_node& node) override;
};

class DependencyPatternList final : public XmlDeserializable {
public:
    std::vector<DependencyPattern> patterns;

    bool deserialize(pugi::xml_node& node) override;
};

class DependencyPluginType final : public XmlDeserializable {
public:
    std::optional<PluginTypeEnum> defaultType;
    DependencyPatternList patterns;

    bool deserialize(pugi::xml_node& node) override;
};

class TypeDescriptor final : public XmlDeserializable {
public:
    DependencyPluginType dependencyType;
    PluginTypeEnum type;

    bool deserialize(pugi::xml_node& node) override;
};

class Image final : public XmlDeserializable {
public:
    std::string path;

    bool deserialize(pugi::xml_node& node) override;
};

class HeaderImage final : public XmlDeserializable {
public:
    std::string path;
    bool showImage = false;
    bool showFade = false;
    int height = 0;

    bool deserialize(pugi::xml_node& node) override;
};

class File final : public XmlDeserializable {
public:
    std::string source;
    std::optional<std::string> destination;
    int priority = 0;
    bool isFolder = false;

    bool deserialize(pugi::xml_node& node) override;
};

class FileList final : public XmlDeserializable {
public:
    std::vector<File> files;

    bool deserialize(pugi::xml_node& node) override;
};

class ConditionalFileInstallPattern final : public XmlDeserializable {
public:
    CompositeDependency dependencies;
    FileList files;

    bool deserialize(pugi::xml_node& node) override;
};

// <flag name="2">On</flag>
class ConditionFlag final : public XmlDeserializable {
public:
    std::string name;
    std::string value;

    bool deserialize(pugi::xml_node& node) override;
};

class ConditionFlagList final : public XmlDeserializable {
public:
    std::vector<ConditionFlag> flags;

    bool deserialize(pugi::xml_node& node) override;
};

class Plugin final : public XmlDeserializable {
public:
    std::string description;
    Image image;
    TypeDescriptor typeDescriptor;
    std::string name;
    ConditionFlagList conditionFlags;
    FileList files;

    bool deserialize(pugi::xml_node& node) override;
};

class PluginList final : public XmlDeserializable, public OrderedContents<Plugin> {
public:
    std::vector<Plugin> plugins;
    OrderTypeEnum order = OrderTypeEnum::Ascending;

    bool deserialize(pugi::xml_node& node) override;
};

class Group final : public XmlDeserializable {
public:
    PluginList plugins;
    std::string name;
    GroupTypeEnum type = SelectAny;

    bool deserialize(pugi::xml_node& node) override;
};

class GroupList final : public XmlDeserializable, public OrderedContents<Group> {
public:
    std::vector<Group> groups;
    OrderTypeEnum order = OrderTypeEnum::Explicit;

    bool deserialize(pugi::xml_node& node) override;
};

class InstallStep final : public XmlDeserializable {
public:
    CompositeDependency visible;
    GroupList optionalFileGroups;
    std::string name;

    bool deserialize(pugi::xml_node& node) override;
};

class ConditionalFileInstall final : public XmlDeserializable {
public:
    std::vector<ConditionalFileInstallPattern> patterns;

    bool deserialize(pugi::xml_node& node) override;
};

class StepList final : public XmlDeserializable, public OrderedContents<InstallStep> {
public:
    std::vector<InstallStep> installSteps;
    OrderTypeEnum order = OrderTypeEnum::Explicit;

    bool deserialize(pugi::xml_node& node) override;
};

class ModuleConfiguration {
public:
    std::string moduleName;
    HeaderImage moduleImage;
    CompositeDependency moduleDependencies;
    FileList requiredInstallFiles;
    StepList installSteps;
    ConditionalFileInstall conditionalFileInstalls;

    // C#-script installers (<csharpScript>) are not supported: the FOMOD
    // spec lets authors ship arbitrary C# instead of declarative steps, and
    // FOMOD Plus itself silently mishandles them. FomodStage detects the
    // presence of the element and aborts with a clear message rather than
    // producing a wrong install.
    std::string csharpScript;
    [[nodiscard]] bool hasCSharpScript() const { return !csharpScript.empty(); }

    // Parse <config> from the given ModuleConfig.xml file. Throws
    // XmlParseException on malformed XML or a missing <config> node.
    bool deserialize(const std::filesystem::path& filePath);
};

// info.xml (the [FOMOD] metadata sidecar shipped alongside ModuleConfig.xml).
class FomodInfoFile {
public:
    bool deserialize(const std::filesystem::path& filePath);

    [[nodiscard]] const std::string& getName() const { return name; }
    [[nodiscard]] const std::string& getAuthor() const { return author; }
    [[nodiscard]] const std::string& getVersion() const { return version; }
    [[nodiscard]] const std::string& getWebsite() const { return website; }
    [[nodiscard]] const std::string& getDescription() const { return description; }
    [[nodiscard]] const std::vector<std::string>& getGroups() const { return groups; }

private:
    std::string name;
    std::string author;
    std::string version;
    std::string website;
    std::string description;
    std::vector<std::string> groups;
};

}  // namespace engine
