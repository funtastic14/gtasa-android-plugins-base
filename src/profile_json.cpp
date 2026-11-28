
#include "profile_json.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace VLS {
namespace ProfileJson {

namespace {

static const size_t MAX_JSON_SIZE = 1024 * 1024;
static const int MAX_JSON_DEPTH = 64;
static const uint32_t MAX_MATRIX_PIXELS = 4096;

struct JsonValue {
    enum Type { Null, Boolean, Number, String, Array, Object };

    Type type = Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& source)
        : m_source(source), m_position(0) {}

    bool Parse(JsonValue& result) {
        SkipWhitespace();

        if (!ParseValue(result, 0))
            return false;

        SkipWhitespace();
        return m_position == m_source.size();
    }

private:
    const std::string& m_source;
    size_t m_position;

    void SkipWhitespace() {
        while (m_position < m_source.size() &&
               std::isspace(static_cast<unsigned char>(
                   m_source[m_position]))) {
            ++m_position;
        }
    }

    bool Consume(char expected) {
        SkipWhitespace();

        if (m_position >= m_source.size() ||
            m_source[m_position] != expected) {
            return false;
        }

        ++m_position;
        return true;
    }

    bool Match(const char* text) {
        size_t length = 0;

        while (text[length] != '\0')
            ++length;

        if (m_position + length > m_source.size())
            return false;

        if (m_source.compare(m_position, length, text) != 0)
            return false;

        m_position += length;
        return true;
    }

    static void AppendUtf8(
        std::string& output,
        uint32_t codepoint
    ) {
        if (codepoint <= 0x7F) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(
                0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(
                0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(
                0x80 | (codepoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(
                0x80 | ((codepoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(
                0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(
                0x80 | (codepoint & 0x3F)));
        }
    }

    bool ParseHex4(uint32_t& result) {
        if (m_position + 4 > m_source.size())
            return false;

        result = 0;

        for (int i = 0; i < 4; ++i) {
            char c = m_source[m_position++];
            result <<= 4;

            if (c >= '0' && c <= '9')
                result |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                result |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                result |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return false;
        }

        return true;
    }

    bool ParseString(std::string& result) {
        if (m_position >= m_source.size() ||
            m_source[m_position] != '"') {
            return false;
        }

        ++m_position;
        result.clear();

        while (m_position < m_source.size()) {
            unsigned char c =
                static_cast<unsigned char>(m_source[m_position++]);

            if (c == '"')
                return true;

            if (c < 0x20)
                return false;

            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }

            if (m_position >= m_source.size())
                return false;

            char escaped = m_source[m_position++];

            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;

                case 'u': {
                    uint32_t codepoint;

                    if (!ParseHex4(codepoint))
                        return false;

                    if (codepoint >= 0xD800 &&
                        codepoint <= 0xDBFF) {
                        if (m_position + 2 > m_source.size() ||
                            m_source[m_position] != '\\' ||
                            m_source[m_position + 1] != 'u') {
                            return false;
                        }

                        m_position += 2;

                        uint32_t low;

                        if (!ParseHex4(low) ||
                            low < 0xDC00 ||
                            low > 0xDFFF) {
                            return false;
                        }

                        codepoint = 0x10000 +
                            ((codepoint - 0xD800) << 10) +
                            (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 &&
                               codepoint <= 0xDFFF) {
                        return false;
                    }

                    AppendUtf8(result, codepoint);
                    break;
                }

                default:
                    return false;
            }
        }

        return false;
    }

    bool ParseNumber(JsonValue& result) {
        size_t start = m_position;

        if (m_position < m_source.size() &&
            m_source[m_position] == '-') {
            ++m_position;
        }

        if (m_position >= m_source.size())
            return false;

        if (m_source[m_position] == '0') {
            ++m_position;
        } else {
            if (m_source[m_position] < '1' ||
                m_source[m_position] > '9') {
                return false;
            }

            while (m_position < m_source.size() &&
                   m_source[m_position] >= '0' &&
                   m_source[m_position] <= '9') {
                ++m_position;
            }
        }

        if (m_position < m_source.size() &&
            m_source[m_position] == '.') {
            ++m_position;
            size_t fractionStart = m_position;

            while (m_position < m_source.size() &&
                   m_source[m_position] >= '0' &&
                   m_source[m_position] <= '9') {
                ++m_position;
            }

            if (fractionStart == m_position)
                return false;
        }

        if (m_position < m_source.size() &&
            (m_source[m_position] == 'e' ||
             m_source[m_position] == 'E')) {
            ++m_position;

            if (m_position < m_source.size() &&
                (m_source[m_position] == '+' ||
                 m_source[m_position] == '-')) {
                ++m_position;
            }

            size_t exponentStart = m_position;

            while (m_position < m_source.size() &&
                   m_source[m_position] >= '0' &&
                   m_source[m_position] <= '9') {
                ++m_position;
            }

            if (exponentStart == m_position)
                return false;
        }

        std::string numberText =
            m_source.substr(start, m_position - start);

        char* end = nullptr;
        double value = std::strtod(numberText.c_str(), &end);

        if (end == nullptr || *end != '\0' || !std::isfinite(value))
            return false;

        result.type = JsonValue::Number;
        result.number = value;
        return true;
    }

    bool ParseArray(JsonValue& result, int depth) {
        if (!Consume('['))
            return false;

        result.type = JsonValue::Array;
        result.array.clear();

        SkipWhitespace();

        if (Consume(']'))
            return true;

        while (true) {
            JsonValue item;

            if (!ParseValue(item, depth + 1))
                return false;

            result.array.push_back(item);

            if (result.array.size() > MAX_MATRIX_PIXELS + 10000)
                return false;

            SkipWhitespace();

            if (Consume(']'))
                return true;

            if (!Consume(','))
                return false;
        }
    }

    bool ParseObject(JsonValue& result, int depth) {
        if (!Consume('{'))
            return false;

        result.type = JsonValue::Object;
        result.object.clear();

        SkipWhitespace();

        if (Consume('}'))
            return true;

        while (true) {
            SkipWhitespace();

            std::string key;

            if (!ParseString(key) || !Consume(':'))
                return false;

            JsonValue value;

            if (!ParseValue(value, depth + 1))
                return false;

            result.object[key] = value;

            SkipWhitespace();

            if (Consume('}'))
                return true;

            if (!Consume(','))
                return false;
        }
    }

    bool ParseValue(JsonValue& result, int depth) {
        if (depth > MAX_JSON_DEPTH)
            return false;

        SkipWhitespace();

        if (m_position >= m_source.size())
            return false;

        char c = m_source[m_position];

        if (c == '{')
            return ParseObject(result, depth);

        if (c == '[')
            return ParseArray(result, depth);

        if (c == '"') {
            result.type = JsonValue::String;
            return ParseString(result.string);
        }

        if (c == 't') {
            if (!Match("true"))
                return false;
            result.type = JsonValue::Boolean;
            result.boolean = true;
            return true;
        }

        if (c == 'f') {
            if (!Match("false"))
                return false;
            result.type = JsonValue::Boolean;
            result.boolean = false;
            return true;
        }

        if (c == 'n') {
            if (!Match("null"))
                return false;
            result.type = JsonValue::Null;
            return true;
        }

        if (c == '-' || (c >= '0' && c <= '9'))
            return ParseNumber(result);

        return false;
    }
};

static const JsonValue* Find(
    const JsonValue& object,
    const char* key
) {
    if (object.type != JsonValue::Object)
        return nullptr;

    auto it = object.object.find(key);

    return it == object.object.end() ? nullptr : &it->second;
}

static bool ReadBool(
    const JsonValue& object,
    const char* key,
    bool& output
) {
    const JsonValue* value = Find(object, key);

    if (value == nullptr)
        return true;

    if (value->type != JsonValue::Boolean)
        return false;

    output = value->boolean;
    return true;
}

static bool ReadNumber(
    const JsonValue& object,
    const char* key,
    double& output
) {
    const JsonValue* value = Find(object, key);

    if (value == nullptr)
        return true;

    if (value->type != JsonValue::Number)
        return false;

    output = value->number;
    return true;
}

static bool ReadInt(
    const JsonValue& object,
    const char* key,
    int& output
) {
    double value = output;

    if (!ReadNumber(object, key, value))
        return false;

    if (value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max() ||
        std::floor(value) != value) {
        return false;
    }

    output = static_cast<int>(value);
    return true;
}

static bool ReadUInt32(
    const JsonValue& object,
    const char* key,
    uint32_t& output
) {
    double value = output;

    if (!ReadNumber(object, key, value))
        return false;

    if (value < 0 ||
        value > std::numeric_limits<uint32_t>::max() ||
        std::floor(value) != value) {
        return false;
    }

    output = static_cast<uint32_t>(value);
    return true;
}

static bool ReadOptionalUInt32(
    const JsonValue& object,
    const char* key,
    uint32_t& output
) {
    if (Find(object, key) == nullptr)
        return true;
    return ReadUInt32(object, key, output);
}

static bool ReadUInt16(
    const JsonValue& object,
    const char* key,
    uint16_t& output
) {
    double value = output;

    if (!ReadNumber(object, key, value))
        return false;

    if (value < 0 || value > 65535 ||
        std::floor(value) != value) {
        return false;
    }

    output = static_cast<uint16_t>(value);
    return true;
}

static bool ReadUInt8(
    const JsonValue& object,
    const char* key,
    uint8_t& output
) {
    double value = output;

    if (!ReadNumber(object, key, value))
        return false;

    if (value < 0 || value > 255 ||
        std::floor(value) != value) {
        return false;
    }

    output = static_cast<uint8_t>(value);
    return true;
}

static bool ReadString(
    const JsonValue& object,
    const char* key,
    std::string& output
) {
    const JsonValue* value = Find(object, key);
    if (value == nullptr)
        return true;
    if (value->type != JsonValue::String || value->string.size() > 128)
        return false;
    output = value->string;
    return true;
}

static bool ReadFloat(
    const JsonValue& object,
    const char* key,
    float& output
) {
    double value = output;

    if (!ReadNumber(object, key, value))
        return false;

    if (value < -std::numeric_limits<float>::max() ||
        value > std::numeric_limits<float>::max()) {
        return false;
    }

    output = static_cast<float>(value);
    return std::isfinite(output);
}

static bool ReadOptionalFloat(
    const JsonValue& object,
    const char* key,
    float& output
) {
    if (Find(object, key) == nullptr)
        return true;
    return ReadFloat(object, key, output);
}

static bool ReadTransform(
    const JsonValue& value,
    LedTransform& transform
) {
    if (value.type != JsonValue::Object)
        return false;

    return
        ReadFloat(value, "x", transform.x) &&
        ReadFloat(value, "y", transform.y) &&
        ReadFloat(value, "z", transform.z) &&
        ReadFloat(value, "rotationX", transform.rotationX) &&
        ReadFloat(value, "rotationY", transform.rotationY) &&
        ReadFloat(value, "rotationZ", transform.rotationZ) &&
        ReadOptionalFloat(value, "curvature", transform.curvature) &&
        ReadOptionalFloat(value, "facingYaw", transform.facingYaw) &&
        ReadOptionalFloat(value, "facingPitch", transform.facingPitch) &&
        ReadFloat(value, "width", transform.width) &&
        ReadFloat(value, "height", transform.height) &&
        ReadFloat(value, "stretchX", transform.stretchX) &&
        ReadFloat(value, "stretchY", transform.stretchY);
}

static bool ReadMatrixPixel(
    const JsonValue& value,
    LedMatrixPixel& pixel
) {
    if (value.type != JsonValue::Object)
        return false;

    int direction = static_cast<int>(pixel.direction);

    if (!ReadBool(value, "enabled", pixel.enabled) ||
        !ReadInt(value, "direction", direction) ||
        direction < 0 ||
        direction > static_cast<int>(LedDirection::CustomAngle)) {
        return false;
    }

    pixel.direction = static_cast<LedDirection>(direction);
    int elementShape = static_cast<int>(pixel.elementShape);
    if (Find(value, "elementShape") != nullptr &&
        (!ReadInt(value, "elementShape", elementShape) ||
         (elementShape != static_cast<int>(LedElementShape::Inherit) &&
          (elementShape < 0 || elementShape > static_cast<int>(LedElementShape::Octagon)))))
        return false;
    pixel.elementShape = static_cast<LedElementShape>(elementShape);

    return
        ReadFloat(value, "customAngle", pixel.customAngle) &&
        ReadUInt8(value, "red", pixel.red) &&
        ReadUInt8(value, "green", pixel.green) &&
        ReadUInt8(value, "blue", pixel.blue) &&
        ReadFloat(value, "brightness", pixel.brightness) &&
        ReadFloat(value, "speed", pixel.speed);
}

static bool ReadMatrix(
    const JsonValue& value,
    LedMatrixConfig& matrix
) {
    if (value.type != JsonValue::Object)
        return false;

    if (!ReadBool(value, "enabled", matrix.enabled) ||
        !ReadUInt16(value, "columns", matrix.columns) ||
        !ReadUInt16(value, "rows", matrix.rows) ||
        !ReadBool(value, "canvasEnabled", matrix.canvasEnabled) ||
        !ReadUInt16(value, "canvasColumns", matrix.canvasColumns) ||
        !ReadUInt16(value, "canvasRows", matrix.canvasRows) ||
        !ReadUInt16(value, "canvasOffsetColumn", matrix.canvasOffsetColumn) ||
        !ReadUInt16(value, "canvasOffsetRow", matrix.canvasOffsetRow)) {
        return false;
    }

    if (matrix.columns == 0 || matrix.rows == 0 ||
        (matrix.canvasEnabled &&
         (matrix.canvasColumns == 0 || matrix.canvasRows == 0 ||
          matrix.canvasColumns > 256 || matrix.canvasRows > 256 ||
          matrix.canvasOffsetColumn > 255 || matrix.canvasOffsetRow > 255 ||
          static_cast<uint32_t>(matrix.canvasOffsetColumn) + matrix.columns > matrix.canvasColumns ||
          static_cast<uint32_t>(matrix.canvasOffsetRow) + matrix.rows > matrix.canvasRows)))
        return false;

    uint32_t total =
        static_cast<uint32_t>(matrix.columns) *
        static_cast<uint32_t>(matrix.rows);

    if (total > MAX_MATRIX_PIXELS)
        return false;

    const JsonValue* pixels = Find(value, "pixels");

    if (pixels != nullptr) {
        if (pixels->type != JsonValue::Array ||
            pixels->array.size() > MAX_MATRIX_PIXELS) {
            return false;
        }

        for (const JsonValue& item : pixels->array) {
            LedMatrixPixel pixel;

            if (!ReadMatrixPixel(item, pixel))
                return false;

            matrix.pixels.push_back(pixel);
        }

        if (!matrix.pixels.empty() &&
            matrix.pixels.size() != total) {
            return false;
        }
    }

    return true;
}

static bool ReadEffect(
    const JsonValue& value,
    LedEffectConfig& effect
) {
    if (value.type != JsonValue::Object)
        return false;

    int type = static_cast<int>(effect.type);

    if (!ReadInt(value, "type", type) ||
        type < 0 ||
        type > static_cast<int>(LedEffectType::CustomAnimation)) {
        return false;
    }

    effect.type = static_cast<LedEffectType>(type);

    const JsonValue* layerOrderValue = Find(value, "layerOrder");
    if (layerOrderValue != nullptr &&
        !ReadUInt8(value, "layerOrder", effect.layerOrder))
        return false;
    int blendMode = static_cast<int>(effect.blendMode);
    if (Find(value, "blendMode") != nullptr &&
        (!ReadInt(value, "blendMode", blendMode) ||
         blendMode < 0 ||
         blendMode > static_cast<int>(LedLayerBlendMode::Additive)))
        return false;
    effect.blendMode = static_cast<LedLayerBlendMode>(blendMode);
    int elementShape = static_cast<int>(effect.elementShape);
    if (Find(value, "elementShape") != nullptr &&
        (!ReadInt(value, "elementShape", elementShape) ||
         elementShape < 0 ||
         elementShape > static_cast<int>(LedElementShape::Octagon)))
        return false;
    effect.elementShape = static_cast<LedElementShape>(elementShape);

    if ((Find(value, "coronaEnabled") && !ReadBool(value, "coronaEnabled", effect.coronaEnabled)) ||
        (Find(value, "glowEnabled") && !ReadBool(value, "glowEnabled", effect.glowEnabled)) ||
        (Find(value, "lightSpreadEnabled") && !ReadBool(value, "lightSpreadEnabled", effect.lightSpreadEnabled)) ||
        !ReadOptionalFloat(value, "coronaIntensity", effect.coronaIntensity) ||
        !ReadOptionalFloat(value, "glowIntensity", effect.glowIntensity) ||
        !ReadOptionalFloat(value, "lightSpreadIntensity", effect.lightSpreadIntensity) ||
        !std::isfinite(effect.coronaIntensity) || effect.coronaIntensity < 0.0f || effect.coronaIntensity > 2.0f ||
        !std::isfinite(effect.glowIntensity) || effect.glowIntensity < 0.0f || effect.glowIntensity > 2.0f ||
        !std::isfinite(effect.lightSpreadIntensity) || effect.lightSpreadIntensity < 0.0f || effect.lightSpreadIntensity > 2.0f)
        return false;

    if (!ReadBool(value, "enabled", effect.enabled) ||
        !ReadUInt8(value, "red", effect.red) ||
        !ReadUInt8(value, "green", effect.green) ||
        !ReadUInt8(value, "blue", effect.blue) ||
        !ReadFloat(value, "brightness", effect.brightness) ||
        !ReadOptionalFloat(value, "opacity", effect.opacity) ||
        !ReadOptionalFloat(value, "speed", effect.speed) ||
        !ReadUInt32(value, "durationMs", effect.durationMs) ||
        effect.durationMs < 100 || effect.durationMs > 60000 ||
        !ReadString(value, "text", effect.text) ||
        !ReadOptionalFloat(value, "textSpacing", effect.textSpacing) ||
        !ReadOptionalFloat(value, "textScale", effect.textScale) ||
        !ReadString(value, "fontName", effect.fontName) ||
        !ReadString(value, "textureName", effect.textureName)) {
        return false;
    }

    if (Find(value, "stroboPanelStackEnabled") &&
        !ReadBool(value, "stroboPanelStackEnabled", effect.stroboPanelStackEnabled))
        return false;
    if (Find(value, "stroboPanelCount")) {
        if (!ReadUInt8(value, "stroboPanelCount", effect.stroboPanelCount) ||
            (effect.stroboPanelCount != 3 && effect.stroboPanelCount != 5 &&
             effect.stroboPanelCount != 7))
            return false;
    }
    if (Find(value, "stroboPanelColumns") &&
        !ReadUInt16(value, "stroboPanelColumns", effect.stroboPanelColumns))
        return false;
    if (Find(value, "stroboPanelRows") &&
        !ReadUInt16(value, "stroboPanelRows", effect.stroboPanelRows))
        return false;
    if (Find(value, "stroboPanelGapRows") &&
        !ReadUInt8(value, "stroboPanelGapRows", effect.stroboPanelGapRows))
        return false;
    int compositionMode = static_cast<int>(effect.stroboCompositionMode);
    if (Find(value, "stroboCompositionMode")) {
        if (!ReadInt(value, "stroboCompositionMode", compositionMode) ||
            compositionMode < 0 ||
            compositionMode > static_cast<int>(PanelMotionCompositionMode::IndependentMotion))
            return false;
        effect.stroboCompositionMode =
            static_cast<PanelMotionCompositionMode>(compositionMode);
    }
    if (Find(value, "stroboFlashEnabled") &&
        !ReadBool(value, "stroboFlashEnabled", effect.stroboFlashEnabled))
        return false;
    if (Find(value, "stroboFlashPeriodMs") &&
        !ReadUInt32(value, "stroboFlashPeriodMs", effect.stroboFlashPeriodMs))
        return false;
    if (effect.stroboPanelColumns == 0 || effect.stroboPanelColumns > 256 ||
        effect.stroboPanelRows == 0 || effect.stroboPanelRows > 64 ||
        effect.stroboPanelGapRows > 16 ||
        effect.stroboFlashPeriodMs < 100 || effect.stroboFlashPeriodMs > 5000)
        return false;
    const JsonValue* stroboPanels = Find(value, "stroboPanels");
    bool legacyUniformPanelDefaults = stroboPanels != nullptr;
    if (effect.type == LedEffectType::StroboWave && stroboPanels == nullptr)
        InitializeStroboWavePanelDefaults(effect);
    if (stroboPanels != nullptr) {
        if (stroboPanels->type != JsonValue::Array ||
            stroboPanels->array.size() != 7)
            return false;
        for (size_t i = 0; i < 7; ++i) {
            const JsonValue& item = stroboPanels->array[i];
            if (item.type != JsonValue::Object)
                return false;
            StroboWavePanelConfig& panel = effect.stroboPanels[i];
            int pattern = static_cast<int>(panel.motionPattern);
            int direction = static_cast<int>(panel.direction);
            int density = static_cast<int>(panel.densityMode);
            if (!ReadBool(item, "enabled", panel.enabled) ||
                !ReadInt(item, "motionPattern", pattern) ||
                pattern < 0 || pattern > static_cast<int>(LedMotionPattern::CenterAnchoredSideSlider) ||
                !ReadInt(item, "direction", direction) ||
                direction < 0 || direction > static_cast<int>(LedColorDirection::EdgeToCenter) ||
                !ReadInt(item, "densityMode", density) ||
                density < 0 || density > static_cast<int>(MotionDensityMode::Custom) ||
                !ReadOptionalFloat(item, "densityX", panel.densityX) ||
                !ReadOptionalFloat(item, "densityY", panel.densityY) ||
                !ReadOptionalFloat(item, "phaseOffset", panel.phaseOffset) ||
                !std::isfinite(panel.densityX) || panel.densityX < 0.25f || panel.densityX > 4.0f ||
                !std::isfinite(panel.densityY) || panel.densityY < 0.25f || panel.densityY > 4.0f ||
                !std::isfinite(panel.phaseOffset) || panel.phaseOffset < -1.0f || panel.phaseOffset > 1.0f)
                return false;
            panel.motionPattern = static_cast<LedMotionPattern>(pattern);
            panel.direction = static_cast<LedColorDirection>(direction);
            panel.densityMode = static_cast<MotionDensityMode>(density);
            legacyUniformPanelDefaults = legacyUniformPanelDefaults &&
                panel.enabled &&
                panel.motionPattern == LedMotionPattern::LeftToRight &&
                panel.direction == LedColorDirection::LeftToRight &&
                panel.densityMode == MotionDensityMode::Adaptive &&
                panel.densityX == 1.0f && panel.densityY == 1.0f &&
                panel.phaseOffset == 0.0f;
        }
        // Profiles saved by the first panel-stack revision serialized seven
        // identical default panels. Migrate that untouched default only;
        // any user-customized panel configuration remains intact.
        if (effect.type == LedEffectType::StroboWave &&
            legacyUniformPanelDefaults)
            InitializeStroboWavePanelDefaults(effect);
    }

    int colorMode = static_cast<int>(effect.colorMode);
    int motionPattern = static_cast<int>(effect.motionPattern);
    if (!ReadInt(value, "colorMode", colorMode) ||
        colorMode < 0 || colorMode > static_cast<int>(LedColorMode::Alternating) ||
        !ReadUInt8(value, "secondaryRed", effect.secondaryRed) ||
        !ReadUInt8(value, "secondaryGreen", effect.secondaryGreen) ||
        !ReadUInt8(value, "secondaryBlue", effect.secondaryBlue)) {
        return false;
    }
    effect.colorMode = static_cast<LedColorMode>(colorMode);
    const JsonValue* tertiaryRedValue = Find(value, "tertiaryRed");
    const JsonValue* tertiaryGreenValue = Find(value, "tertiaryGreen");
    const JsonValue* tertiaryBlueValue = Find(value, "tertiaryBlue");
    if ((tertiaryRedValue && !ReadUInt8(value, "tertiaryRed", effect.tertiaryRed)) ||
        (tertiaryGreenValue && !ReadUInt8(value, "tertiaryGreen", effect.tertiaryGreen)) ||
        (tertiaryBlueValue && !ReadUInt8(value, "tertiaryBlue", effect.tertiaryBlue))) return false;
    int colorDirection = static_cast<int>(effect.colorDirection);
    if (Find(value, "colorDirection") &&
        (!ReadInt(value, "colorDirection", colorDirection) || colorDirection < 0 ||
         colorDirection > static_cast<int>(LedColorDirection::EdgeToCenter))) return false;
    effect.colorDirection = static_cast<LedColorDirection>(colorDirection);
    if (!ReadOptionalFloat(value, "colorSpread", effect.colorSpread) ||
        !std::isfinite(effect.colorSpread) || effect.colorSpread < 0.1f || effect.colorSpread > 8.0f)
        return false;
    // Version 2 stores only Original, Round and Sleepy. Legacy enum
    // values 3 and 6 map to Round and Sleepy; removed shapes fall back safely.
    int eyeShape = static_cast<int>(effect.eyeShape);
    int eyeShapeVersion = 1;
    if (Find(value, "eyeShapeVersion") &&
        (!ReadInt(value, "eyeShapeVersion", eyeShapeVersion) ||
         eyeShapeVersion < 1 || eyeShapeVersion > 2))
        return false;
    if (Find(value, "eyeShape")) {
        if (!ReadInt(value, "eyeShape", eyeShape))
            return false;
        if (eyeShapeVersion >= 2) {
            if (eyeShape < 0 || eyeShape > static_cast<int>(EyeShape::Sleepy))
                return false;
        } else {
            eyeShape = eyeShape == 3 ? static_cast<int>(EyeShape::Round) :
                (eyeShape == 6 ? static_cast<int>(EyeShape::Sleepy) :
                                 static_cast<int>(EyeShape::Original));
        }
    }
    // Preserve the old dedicated Sleepy expression as the new Sleepy
    // shape only when no explicit shape was saved.
    if (!Find(value, "eyeShape") && Find(value, "eyeExpression")) {
        int legacyExpression = 0;
        if (!ReadInt(value, "eyeExpression", legacyExpression))
            return false;
        if (legacyExpression == 5)
            eyeShape = static_cast<int>(EyeShape::Sleepy);
    }
    effect.eyeShape = static_cast<EyeShape>(eyeShape);

    // Eye motion schema v2 separates gaze directions from retired expression
    // and iris-motif presets. Legacy values are migrated without reviving them.
    int eyeMotionVersion = 1;
    int eyeMotion = static_cast<int>(effect.eyeMotion);
    if (Find(value, "eyeMotionVersion") &&
        (!ReadInt(value, "eyeMotionVersion", eyeMotionVersion) ||
         eyeMotionVersion < 1 || eyeMotionVersion > 2))
        return false;
    if (Find(value, "eyeMotion")) {
        if (!ReadInt(value, "eyeMotion", eyeMotion) || eyeMotion < 0)
            return false;
        if (eyeMotionVersion >= 2) {
            if (eyeMotion > static_cast<int>(EyeMotion::Wink))
                return false;
        } else {
            switch (eyeMotion) {
            case 0: case 1: case 2: case 3: case 4: break;
            case 5: eyeMotion = static_cast<int>(EyeMotion::Scan); break;
            case 6: eyeMotion = static_cast<int>(EyeMotion::Wink); break;
            case 7: eyeMotion = static_cast<int>(EyeMotion::Shake); break;
            default: eyeMotion = static_cast<int>(EyeMotion::Idle); break;
            }
        }
    }
    effect.eyeMotion = static_cast<EyeMotion>(eyeMotion);
    int eyeMotionPlayback = static_cast<int>(effect.eyeMotionPlayback);
    if (Find(value, "eyeMotionPlayback") &&
        (!ReadInt(value, "eyeMotionPlayback", eyeMotionPlayback) ||
         eyeMotionPlayback < 0 ||
         eyeMotionPlayback > static_cast<int>(MotionPlaybackMode::Random)))
        return false;
    effect.eyeMotionPlayback = static_cast<MotionPlaybackMode>(eyeMotionPlayback);
    if (!ReadOptionalUInt32(value, "eyeMotionIntervalMs", effect.eyeMotionIntervalMs) ||
        effect.eyeMotionIntervalMs < 500 || effect.eyeMotionIntervalMs > 30000 ||
        !ReadOptionalUInt32(value, "eyeBlinkIntervalMs", effect.eyeBlinkIntervalMs) ||
        effect.eyeBlinkIntervalMs < 500 || effect.eyeBlinkIntervalMs > 8000)
        return false;

    const char* eyePaletteKeys[] = {
        "eyeIrisRed", "eyeIrisGreen", "eyeIrisBlue",
        "eyePupilRed", "eyePupilGreen", "eyePupilBlue"
    };
    uint8_t* eyePaletteValues[] = {
        &effect.eyeIrisRed, &effect.eyeIrisGreen, &effect.eyeIrisBlue,
        &effect.eyePupilRed, &effect.eyePupilGreen, &effect.eyePupilBlue
    };
    for (size_t i = 0; i < sizeof(eyePaletteKeys) / sizeof(eyePaletteKeys[0]); ++i) {
        if (Find(value, eyePaletteKeys[i]) &&
            !ReadUInt8(value, eyePaletteKeys[i], *eyePaletteValues[i]))
            return false;
    }
    if (!ReadOptionalFloat(value, "eyeTextureIrisCenterX", effect.eyeTextureIrisCenterX) ||
        !ReadOptionalFloat(value, "eyeTextureIrisCenterY", effect.eyeTextureIrisCenterY) ||
        !ReadOptionalFloat(value, "eyeTextureIrisRadiusX", effect.eyeTextureIrisRadiusX) ||
        !ReadOptionalFloat(value, "eyeTextureIrisRadiusY", effect.eyeTextureIrisRadiusY) ||
        !ReadOptionalFloat(value, "eyeTexturePupilCenterX", effect.eyeTexturePupilCenterX) ||
        !ReadOptionalFloat(value, "eyeTexturePupilCenterY", effect.eyeTexturePupilCenterY) ||
        !ReadOptionalFloat(value, "eyeTexturePupilRadiusX", effect.eyeTexturePupilRadiusX) ||
        !ReadOptionalFloat(value, "eyeTexturePupilRadiusY", effect.eyeTexturePupilRadiusY) ||
        !std::isfinite(effect.eyeTextureIrisCenterX) ||
        !std::isfinite(effect.eyeTextureIrisCenterY) ||
        !std::isfinite(effect.eyeTextureIrisRadiusX) ||
        !std::isfinite(effect.eyeTextureIrisRadiusY) ||
        !std::isfinite(effect.eyeTexturePupilCenterX) ||
        !std::isfinite(effect.eyeTexturePupilCenterY) ||
        !std::isfinite(effect.eyeTexturePupilRadiusX) ||
        !std::isfinite(effect.eyeTexturePupilRadiusY) ||
        effect.eyeTextureIrisCenterX < 0.0f || effect.eyeTextureIrisCenterX > 1.0f ||
        effect.eyeTextureIrisCenterY < 0.0f || effect.eyeTextureIrisCenterY > 1.0f ||
        effect.eyeTextureIrisRadiusX < 0.02f || effect.eyeTextureIrisRadiusX > 0.50f ||
        effect.eyeTextureIrisRadiusY < 0.02f || effect.eyeTextureIrisRadiusY > 0.50f ||
        effect.eyeTexturePupilCenterX < 0.0f || effect.eyeTexturePupilCenterX > 1.0f ||
        effect.eyeTexturePupilCenterY < 0.0f || effect.eyeTexturePupilCenterY > 1.0f ||
        effect.eyeTexturePupilRadiusX < 0.01f || effect.eyeTexturePupilRadiusX > 0.30f ||
        effect.eyeTexturePupilRadiusY < 0.01f || effect.eyeTexturePupilRadiusY > 0.30f)
        return false;

    // Texture selection is a separate controller from eye gaze playback.
    int texturePlayback = static_cast<int>(effect.eyeTexturePlayback);
    if (Find(value, "eyeTexturePlayback") &&
        (!ReadInt(value, "eyeTexturePlayback", texturePlayback) ||
         texturePlayback < 0 ||
         texturePlayback > static_cast<int>(EyeTexturePlaybackMode::Random)))
        return false;
    effect.eyeTexturePlayback = static_cast<EyeTexturePlaybackMode>(texturePlayback);
    if (!ReadOptionalUInt32(value, "eyeTextureIntervalMs", effect.eyeTextureIntervalMs) ||
        effect.eyeTextureIntervalMs < 500 || effect.eyeTextureIntervalMs > 60000)
        return false;
    int textureIndex = static_cast<int>(effect.eyeTextureIndex);
    if (Find(value, "eyeTextureIndex") &&
        (!ReadInt(value, "eyeTextureIndex", textureIndex) ||
         textureIndex < 0 || textureIndex > 31))
        return false;
    effect.eyeTextureIndex = static_cast<uint8_t>(textureIndex);

    const JsonValue* eyeTexturesValue = Find(value, "eyeTextures");
    if (eyeTexturesValue != nullptr) {
        if (eyeTexturesValue->type != JsonValue::Array ||
            eyeTexturesValue->array.size() > 32)
            return false;
        effect.eyeTextures.clear();
        for (const JsonValue& item : eyeTexturesValue->array) {
            if (item.type != JsonValue::Object)
                return false;
            EyeTextureDesign design;
            if (!ReadString(item, "textureName", design.textureName) ||
                design.textureName.empty() || design.textureName.size() > 32)
                return false;
            if (Find(item, "name")) {
                if (!ReadString(item, "name", design.name) ||
                    design.name.empty() || design.name.size() > 32)
                    return false;
            } else {
                design.name = design.textureName;
            }
            if (!ReadOptionalFloat(item, "irisCenterX", design.irisCenterX) ||
                !ReadOptionalFloat(item, "irisCenterY", design.irisCenterY) ||
                !ReadOptionalFloat(item, "irisRadiusX", design.irisRadiusX) ||
                !ReadOptionalFloat(item, "irisRadiusY", design.irisRadiusY) ||
                !ReadOptionalFloat(item, "pupilCenterX", design.pupilCenterX) ||
                !ReadOptionalFloat(item, "pupilCenterY", design.pupilCenterY) ||
                !ReadOptionalFloat(item, "pupilRadiusX", design.pupilRadiusX) ||
                !ReadOptionalFloat(item, "pupilRadiusY", design.pupilRadiusY))
                return false;
            if (Find(item, "mappingConfigured") != nullptr) {
                if (!ReadBool(item, "mappingConfigured", design.mappingConfigured))
                    return false;
            } else {
                design.mappingConfigured =
                    Find(item, "irisCenterX") != nullptr ||
                    Find(item, "irisCenterY") != nullptr ||
                    Find(item, "irisRadiusX") != nullptr ||
                    Find(item, "irisRadiusY") != nullptr ||
                    Find(item, "pupilCenterX") != nullptr ||
                    Find(item, "pupilCenterY") != nullptr ||
                    Find(item, "pupilRadiusX") != nullptr ||
                    Find(item, "pupilRadiusY") != nullptr;
            }
            const float values[] = {
                design.irisCenterX, design.irisCenterY,
                design.irisRadiusX, design.irisRadiusY,
                design.pupilCenterX, design.pupilCenterY,
                design.pupilRadiusX, design.pupilRadiusY
            };
            for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
                if (!std::isfinite(values[i])) return false;
            if (design.irisCenterX < 0.0f || design.irisCenterX > 1.0f ||
                design.irisCenterY < 0.0f || design.irisCenterY > 1.0f ||
                design.irisRadiusX < 0.02f || design.irisRadiusX > 0.50f ||
                design.irisRadiusY < 0.02f || design.irisRadiusY > 0.50f ||
                design.pupilCenterX < 0.0f || design.pupilCenterX > 1.0f ||
                design.pupilCenterY < 0.0f || design.pupilCenterY > 1.0f ||
                design.pupilRadiusX < 0.01f || design.pupilRadiusX > 0.30f ||
                design.pupilRadiusY < 0.01f || design.pupilRadiusY > 0.30f)
                return false;
            effect.eyeTextures.push_back(design);
        }
        if (!effect.eyeTextures.empty() &&
            effect.eyeTextureIndex >= effect.eyeTextures.size())
            effect.eyeTextureIndex = 0;
    }

    const JsonValue* textureSequenceValue = Find(value, "eyeTextureSequence");
    if (textureSequenceValue != nullptr) {
        if (textureSequenceValue->type != JsonValue::Array ||
            textureSequenceValue->array.size() > 32)
            return false;
        effect.eyeTextureSequence.clear();
        for (const JsonValue& step : textureSequenceValue->array) {
            if (step.type != JsonValue::Number ||
                step.number < 0.0 || step.number > 31.0 ||
                std::floor(step.number) != step.number)
                return false;
            effect.eyeTextureSequence.push_back(static_cast<uint8_t>(step.number));
        }
    }

    if (!ReadInt(value, "motionPattern", motionPattern) || motionPattern < 0 || motionPattern > static_cast<int>(LedMotionPattern::CenterAnchoredSideSlider)) return false;
    effect.motionPattern = static_cast<LedMotionPattern>(motionPattern);
    // Migrate profiles saved by the earlier modifier-based UI. A legacy
    // modifier becomes a first-class pattern; diagonal combinations retain
    // their intended diagonal character.
    const char* modifierKey = Find(value, "motionModifier") ? "motionModifier" : "motionTransition";
    int legacyModifier = 0;
    if (Find(value, modifierKey)) {
        if (!ReadInt(value, modifierKey, legacyModifier) || legacyModifier < 0 || legacyModifier > 2)
            return false;
        if (motionPattern <= static_cast<int>(LedMotionPattern::OpposingArrows)) {
            if (legacyModifier == 1) {
                effect.motionPattern = motionPattern == static_cast<int>(LedMotionPattern::Diagonal)
                    ? LedMotionPattern::DiagonalCollision : LedMotionPattern::CenterCollision;
            } else if (legacyModifier == 2) {
                effect.motionPattern = motionPattern == static_cast<int>(LedMotionPattern::Diagonal)
                    ? LedMotionPattern::DiagonalCenterBurst : LedMotionPattern::CenterBurst;
            }
        }
    }
    int motionPlayback = static_cast<int>(effect.motionPlayback);
    if (!ReadInt(value, "motionPlayback", motionPlayback) || motionPlayback < 0 || motionPlayback > static_cast<int>(MotionPlaybackMode::Random)) return false;
    effect.motionPlayback = static_cast<MotionPlaybackMode>(motionPlayback);
    if (!ReadUInt32(value, "motionIntervalMs", effect.motionIntervalMs) || effect.motionIntervalMs < 500 || effect.motionIntervalMs > 30000) return false;
    int textMotion = static_cast<int>(effect.textMotion);
    if (!ReadInt(value, "textMotion", textMotion) || textMotion < 0 || textMotion > static_cast<int>(RunningTextMotion::Static)) return false;
    effect.textMotion = static_cast<RunningTextMotion>(textMotion);

    const JsonValue* matrix = Find(value, "matrix");

    if (matrix != nullptr &&
        !ReadMatrix(*matrix, effect.matrix)) {
        return false;
    }
    // Strobo Wave is always a dedicated 3/5/7-panel matrix effect.
    // Older one-panel profiles are migrated to the stacked layout on load.
    if (effect.type == LedEffectType::StroboWave) {
        effect.stroboPanelStackEnabled = true;
        const uint32_t totalRows =
            static_cast<uint32_t>(effect.stroboPanelCount) * effect.stroboPanelRows +
            static_cast<uint32_t>(effect.stroboPanelCount - 1u) * effect.stroboPanelGapRows;
        const uint32_t totalPixels =
            static_cast<uint32_t>(effect.stroboPanelColumns) * totalRows;
        if (totalRows == 0 || totalRows > 256 || totalPixels > MAX_MATRIX_PIXELS)
            return false;
        effect.matrix.enabled = true;
        effect.matrix.columns = effect.stroboPanelColumns;
        effect.matrix.rows = static_cast<uint16_t>(totalRows);
        effect.matrix.pixels.resize(totalPixels);
        // The stack itself is one automatically shared logical canvas.
        effect.matrix.canvasEnabled = true;
        effect.matrix.canvasColumns = effect.stroboPanelColumns;
        effect.matrix.canvasRows = static_cast<uint16_t>(totalRows);
        effect.matrix.canvasOffsetColumn = 0;
        effect.matrix.canvasOffsetRow = 0;
    }
    if (!ReadBool(value, "transparentMatrixBackground", effect.transparentMatrixBackground))
        return false;

    return true;
}

static bool ReadPlacement(
    const JsonValue& value,
    LedPlacement& placement
) {
    if (value.type != JsonValue::Object)
        return false;

    if (!ReadUInt32(value, "id", placement.id))
        return false;

    int zone = static_cast<int>(placement.zone);

    if (!ReadInt(value, "zone", zone) ||
        zone < 0 ||
        zone > static_cast<int>(LedZone::Roof)) {
        return false;
    }

    placement.zone = static_cast<LedZone>(zone);
    if (Find(value, "enabled") != nullptr &&
        !ReadBool(value, "enabled", placement.enabled))
        return false;

    if (!ReadUInt8(value, "animationMode", placement.animationMode) ||
        placement.animationMode > 4 ||
        !ReadUInt32(value, "animationIntervalMs", placement.animationIntervalMs) ||
        placement.animationIntervalMs < 100 ||
        placement.animationIntervalMs > 30000 ||
        !ReadUInt32(value, "activeEffectIndex", placement.activeEffectIndex))
        return false;

    const JsonValue* transform = Find(value, "transform");

    if (transform != nullptr &&
        !ReadTransform(*transform, placement.transform)) {
        return false;
    }

    const JsonValue* effects = Find(value, "effects");

    if (effects != nullptr) {
        if (effects->type != JsonValue::Array)
            return false;

        for (const JsonValue& item : effects->array) {
            LedEffectConfig effect;

            if (!ReadEffect(item, effect))
                return false;

            placement.effects.push_back(effect);
        }
    }

    return true;
}

static bool ReadSequenceStep(
    const JsonValue& value,
    EffectSequenceStep& step
) {
    if (value.type != JsonValue::Object)
        return false;

    return
        ReadUInt32(value, "placementId", step.placementId) &&
        ReadUInt32(value, "effectIndex", step.effectIndex) &&
        ReadUInt32(value, "durationMs", step.durationMs);
}

static bool ReadSequence(
    const JsonValue& value,
    EffectSequence& sequence
) {
    if (value.type != JsonValue::Object)
        return false;

    if (!ReadBool(value, "enabled", sequence.enabled) ||
        !ReadBool(value, "loop", sequence.loop)) {
        return false;
    }

    int transitionMode = static_cast<int>(sequence.transitionMode);
    int transitionDirection = static_cast<int>(sequence.transitionDirection);
    if (Find(value, "transitionMode") &&
        (!ReadInt(value, "transitionMode", transitionMode) ||
         transitionMode < 0 || transitionMode > static_cast<int>(EffectTransitionMode::PixelDissolve)))
        return false;
    if (Find(value, "transitionDirection") &&
        (!ReadInt(value, "transitionDirection", transitionDirection) ||
         transitionDirection < 0 || transitionDirection > static_cast<int>(EffectTransitionDirection::EdgeToCenter)))
        return false;
    sequence.transitionMode = static_cast<EffectTransitionMode>(transitionMode);
    sequence.transitionDirection = static_cast<EffectTransitionDirection>(transitionDirection);

    const JsonValue* steps = Find(value, "steps");

    if (steps != nullptr) {
        if (steps->type != JsonValue::Array)
            return false;

        for (const JsonValue& item : steps->array) {
            EffectSequenceStep step;

            if (!ReadSequenceStep(item, step))
                return false;

            sequence.steps.push_back(step);
        }
    }

    return true;
}

static bool ValidatePixel(const LedMatrixPixel& pixel) {
    int direction = static_cast<int>(pixel.direction);
    int shape = static_cast<int>(pixel.elementShape);

    return
        (shape == static_cast<int>(LedElementShape::Inherit) ||
         (shape >= 0 && shape <= static_cast<int>(LedElementShape::Octagon))) &&
        direction >= 0 &&
        direction <= static_cast<int>(LedDirection::CustomAngle) &&
        std::isfinite(pixel.customAngle) &&
        std::isfinite(pixel.brightness) &&
        pixel.brightness >= 0.0f &&
        pixel.brightness <= 1.0f &&
        std::isfinite(pixel.speed) &&
        pixel.speed > 0.0f;
}

static bool ValidateProfileReferences(
    const VehicleProfile& profile
) {
    for (size_t i = 0; i < profile.placements.size(); ++i) {
        const LedPlacement& placement = profile.placements[i];

        if (placement.id == 0)
            return false;

        for (size_t j = i + 1; j < profile.placements.size(); ++j) {
            if (profile.placements[j].id == placement.id)
                return false;
        }

        int zone = static_cast<int>(placement.zone);

        if (zone < 0 ||
            zone > static_cast<int>(LedZone::Roof) ||
            placement.animationMode > 4 ||
            placement.animationIntervalMs < 100 ||
            placement.animationIntervalMs > 30000 ||
            (!placement.effects.empty() && placement.activeEffectIndex >= placement.effects.size())) {
            return false;
        }

        const LedTransform& transform = placement.transform;

        if (!std::isfinite(transform.x) ||
            !std::isfinite(transform.y) ||
            !std::isfinite(transform.z) ||
            !std::isfinite(transform.rotationX) ||
            !std::isfinite(transform.rotationY) ||
            !std::isfinite(transform.rotationZ) ||
            !std::isfinite(transform.curvature) ||
            !std::isfinite(transform.facingYaw) ||
            !std::isfinite(transform.facingPitch) ||
            !std::isfinite(transform.width) ||
            !std::isfinite(transform.height) ||
            !std::isfinite(transform.stretchX) ||
            !std::isfinite(transform.stretchY) ||
            transform.width <= 0.0f ||
            transform.height <= 0.0f ||
            transform.stretchX <= 0.0f ||
            transform.stretchY <= 0.0f) {
            return false;
        }

        for (const LedEffectConfig& effect : placement.effects) {
            int type = static_cast<int>(effect.type);

            if (type < 0 ||
                type > static_cast<int>(LedEffectType::CustomAnimation) ||
                !std::isfinite(effect.brightness) ||
                effect.brightness < 0.0f ||
                effect.brightness > 1.0f ||
                !std::isfinite(effect.opacity) ||
                effect.opacity < 0.0f ||
                effect.opacity > 1.0f ||
                static_cast<int>(effect.colorMode) < 0 ||
                static_cast<int>(effect.colorMode) > static_cast<int>(LedColorMode::Alternating) ||
                static_cast<int>(effect.colorDirection) < 0 ||
                static_cast<int>(effect.colorDirection) > static_cast<int>(LedColorDirection::EdgeToCenter) ||
                static_cast<int>(effect.blendMode) < 0 ||
                static_cast<int>(effect.blendMode) > static_cast<int>(LedLayerBlendMode::Additive) ||
                !std::isfinite(effect.colorSpread) ||
                effect.colorSpread < 0.1f || effect.colorSpread > 8.0f ||
                !std::isfinite(effect.speed) ||
                effect.speed <= 0.0f ||
                effect.durationMs < 100 ||
                effect.durationMs > 60000 ||
                !std::isfinite(effect.textSpacing) ||
                effect.textSpacing < 0.25f ||
                effect.textSpacing > 4.0f ||
                !std::isfinite(effect.textScale) ||
                effect.textScale < 0.05f || effect.textScale > 3.0f ||
                effect.text.size() > 128) {
                return false;
            }

            const LedMatrixConfig& matrix = effect.matrix;

            if (matrix.columns == 0 || matrix.rows == 0 ||
                (matrix.canvasEnabled &&
                 (matrix.canvasColumns == 0 || matrix.canvasRows == 0 ||
                  matrix.canvasColumns > 256 || matrix.canvasRows > 256 ||
                  matrix.canvasOffsetColumn > 255 || matrix.canvasOffsetRow > 255 ||
                  static_cast<uint32_t>(matrix.canvasOffsetColumn) + matrix.columns > matrix.canvasColumns ||
                  static_cast<uint32_t>(matrix.canvasOffsetRow) + matrix.rows > matrix.canvasRows)))
                return false;

            uint32_t total =
                static_cast<uint32_t>(matrix.columns) *
                static_cast<uint32_t>(matrix.rows);

            if (total > MAX_MATRIX_PIXELS ||
                matrix.pixels.size() > MAX_MATRIX_PIXELS) {
                return false;
            }

            if (!matrix.pixels.empty() &&
                matrix.pixels.size() != total) {
                return false;
            }

            for (const LedMatrixPixel& pixel : matrix.pixels) {
                if (!ValidatePixel(pixel))
                    return false;
            }
        }
    }

    for (const EffectSequence& sequence : profile.sequences) {
        for (const EffectSequenceStep& step : sequence.steps) {
            if (step.placementId == 0 || step.durationMs == 0)
                return false;

            const LedPlacement* targetPlacement = nullptr;

            for (const LedPlacement& placement : profile.placements) {
                if (placement.id == step.placementId) {
                    targetPlacement = &placement;
                    break;
                }
            }

            if (targetPlacement == nullptr ||
                step.effectIndex >= targetPlacement->effects.size()) {
                return false;
            }
        }
    }

    return true;
}

static bool AppendFloat(std::string& output, float value) {
    if (!std::isfinite(value))
        return false;

    char buffer[48];

    int length = std::snprintf(
        buffer,
        sizeof(buffer),
        "%.9g",
        static_cast<double>(value)
    );

    if (length <= 0 ||
        static_cast<size_t>(length) >= sizeof(buffer)) {
        return false;
    }

    output.append(buffer, static_cast<size_t>(length));
    return true;
}

static void AppendUInt(std::string& output, uint32_t value) {
    output += std::to_string(value);
}

static void AppendInt(std::string& output, int value) {
    output += std::to_string(value);
}

static bool AppendTransform(
    std::string& output,
    const LedTransform& transform
) {
    output += "{\"x\":";

    if (!AppendFloat(output, transform.x)) return false;

    output += ",\"y\":";
    if (!AppendFloat(output, transform.y)) return false;

    output += ",\"z\":";
    if (!AppendFloat(output, transform.z)) return false;

    output += ",\"rotationX\":";
    if (!AppendFloat(output, transform.rotationX)) return false;

    output += ",\"rotationY\":";
    if (!AppendFloat(output, transform.rotationY)) return false;

    output += ",\"rotationZ\":";
    if (!AppendFloat(output, transform.rotationZ)) return false;

    output += ",\"curvature\":";
    if (!AppendFloat(output, transform.curvature)) return false;

    output += ",\"facingYaw\":";
    if (!AppendFloat(output, transform.facingYaw)) return false;

    output += ",\"facingPitch\":";
    if (!AppendFloat(output, transform.facingPitch)) return false;

    output += ",\"width\":";
    if (!AppendFloat(output, transform.width)) return false;

    output += ",\"height\":";
    if (!AppendFloat(output, transform.height)) return false;

    output += ",\"stretchX\":";
    if (!AppendFloat(output, transform.stretchX)) return false;

    output += ",\"stretchY\":";
    if (!AppendFloat(output, transform.stretchY)) return false;

    output += "}";
    return true;
}

static bool AppendMatrixPixel(
    std::string& output,
    const LedMatrixPixel& pixel
) {
    if (!ValidatePixel(pixel))
        return false;

    const int shape = static_cast<int>(pixel.elementShape);
    if (shape != static_cast<int>(LedElementShape::Inherit) &&
        (shape < 0 || shape > static_cast<int>(LedElementShape::Octagon)))
        return false;

    output += "{\"enabled\":";
    output += pixel.enabled ? "true" : "false";
    output += ",\"elementShape\":";
    AppendInt(output, shape);

    output += ",\"direction\":";
    AppendInt(output, static_cast<int>(pixel.direction));

    output += ",\"customAngle\":";
    if (!AppendFloat(output, pixel.customAngle)) return false;

    output += ",\"red\":";
    AppendUInt(output, pixel.red);

    output += ",\"green\":";
    AppendUInt(output, pixel.green);

    output += ",\"blue\":";
    AppendUInt(output, pixel.blue);

    output += ",\"brightness\":";
    if (!AppendFloat(output, pixel.brightness)) return false;

    output += ",\"speed\":";
    if (!AppendFloat(output, pixel.speed)) return false;

    output += "}";
    return true;
}

static bool AppendMatrix(
    std::string& output,
    const LedMatrixConfig& matrix
) {
    output += "{\"enabled\":";
    output += matrix.enabled ? "true" : "false";

    output += ",\"columns\":";
    AppendUInt(output, matrix.columns);

    output += ",\"rows\":";
    AppendUInt(output, matrix.rows);
    output += ",\"canvasEnabled\":";
    output += matrix.canvasEnabled ? "true" : "false";
    output += ",\"canvasColumns\":";
    AppendUInt(output, matrix.canvasColumns);
    output += ",\"canvasRows\":";
    AppendUInt(output, matrix.canvasRows);
    output += ",\"canvasOffsetColumn\":";
    AppendUInt(output, matrix.canvasOffsetColumn);
    output += ",\"canvasOffsetRow\":";
    AppendUInt(output, matrix.canvasOffsetRow);

    output += ",\"pixels\":[";

    for (size_t i = 0; i < matrix.pixels.size(); ++i) {
        if (i > 0)
            output += ",";

        if (!AppendMatrixPixel(output, matrix.pixels[i]))
            return false;
    }

    output += "]}";
    return true;
}

static bool AppendJsonString(std::string& output, const std::string& value) {
    if (value.size() > 128)
        return false;
    output.push_back('"');
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (ch < 0x20)
                return false;
            output.push_back(static_cast<char>(ch));
        }
    }
    output.push_back('"');
    return true;
}

static bool AppendEffect(
    std::string& output,
    const LedEffectConfig& effect
) {
    if (static_cast<int>(effect.type) < 0 ||
        static_cast<int>(effect.type) >
            static_cast<int>(LedEffectType::CustomAnimation) ||
        static_cast<int>(effect.elementShape) < 0 ||
        static_cast<int>(effect.elementShape) >
            static_cast<int>(LedElementShape::Octagon) ||
        (effect.stroboPanelCount != 3 && effect.stroboPanelCount != 5 &&
         effect.stroboPanelCount != 7) ||
        effect.stroboPanelColumns == 0 || effect.stroboPanelColumns > 256 ||
        effect.stroboPanelRows == 0 || effect.stroboPanelRows > 64 ||
        effect.stroboPanelGapRows > 16 ||
        static_cast<int>(effect.stroboCompositionMode) < 0 ||
        static_cast<int>(effect.stroboCompositionMode) >
            static_cast<int>(PanelMotionCompositionMode::IndependentMotion) ||
        effect.stroboFlashPeriodMs < 100 || effect.stroboFlashPeriodMs > 5000) {
        return false;
    }
    for (const StroboWavePanelConfig& panel : effect.stroboPanels) {
        if (static_cast<int>(panel.motionPattern) < 0 ||
            static_cast<int>(panel.motionPattern) >
                static_cast<int>(LedMotionPattern::CenterAnchoredSideSlider) ||
            static_cast<int>(panel.direction) < 0 ||
            static_cast<int>(panel.direction) >
                static_cast<int>(LedColorDirection::EdgeToCenter) ||
            static_cast<int>(panel.densityMode) < 0 ||
            static_cast<int>(panel.densityMode) >
                static_cast<int>(MotionDensityMode::Custom))
            return false;
    }

    output += "{\"type\":";
    AppendInt(output, static_cast<int>(effect.type));

    output += ",\"enabled\":";
    output += effect.enabled ? "true" : "false";

    output += ",\"red\":";
    AppendUInt(output, effect.red);

    output += ",\"green\":";
    AppendUInt(output, effect.green);

    output += ",\"blue\":";
    AppendUInt(output, effect.blue);

    output += ",\"brightness\":";
    if (!AppendFloat(output, effect.brightness)) return false;

    output += ",\"opacity\":";
    if (!AppendFloat(output, effect.opacity)) return false;
    output += ",\"layerOrder\":";
    AppendUInt(output, effect.layerOrder);
    output += ",\"blendMode\":";
    AppendInt(output, static_cast<int>(effect.blendMode));
    output += ",\"elementShape\":";
    AppendInt(output, static_cast<int>(effect.elementShape));
    output += ",\"coronaEnabled\":";
    output += effect.coronaEnabled ? "true" : "false";
    output += ",\"glowEnabled\":";
    output += effect.glowEnabled ? "true" : "false";
    output += ",\"lightSpreadEnabled\":";
    output += effect.lightSpreadEnabled ? "true" : "false";
    output += ",\"coronaIntensity\":";
    if (!AppendFloat(output, effect.coronaIntensity)) return false;
    output += ",\"glowIntensity\":";
    if (!AppendFloat(output, effect.glowIntensity)) return false;
    output += ",\"lightSpreadIntensity\":";
    if (!AppendFloat(output, effect.lightSpreadIntensity)) return false;

    output += ",\"colorMode\":";
    AppendInt(output, static_cast<int>(effect.colorMode));
    output += ",\"motionPattern\":";
    AppendInt(output, static_cast<int>(effect.motionPattern));
    output += ",\"motionPlayback\":";
    AppendInt(output, static_cast<int>(effect.motionPlayback));
    output += ",\"motionIntervalMs\":";
    AppendUInt(output, effect.motionIntervalMs);
    output += ",\"stroboPanelStackEnabled\":";
    output += effect.stroboPanelStackEnabled ? "true" : "false";
    output += ",\"stroboPanelCount\":";
    AppendUInt(output, effect.stroboPanelCount);
    output += ",\"stroboPanelColumns\":";
    AppendUInt(output, effect.stroboPanelColumns);
    output += ",\"stroboPanelRows\":";
    AppendUInt(output, effect.stroboPanelRows);
    output += ",\"stroboPanelGapRows\":";
    AppendUInt(output, effect.stroboPanelGapRows);
    output += ",\"stroboCompositionMode\":";
    AppendInt(output, static_cast<int>(effect.stroboCompositionMode));
    output += ",\"stroboFlashEnabled\":";
    output += effect.stroboFlashEnabled ? "true" : "false";
    output += ",\"stroboFlashPeriodMs\":";
    AppendUInt(output, effect.stroboFlashPeriodMs);
    output += ",\"stroboPanels\":[";
    for (size_t i = 0; i < effect.stroboPanels.size(); ++i) {
        if (i) output += ",";
        const StroboWavePanelConfig& panel = effect.stroboPanels[i];
        if (!std::isfinite(panel.densityX) || !std::isfinite(panel.densityY) ||
            !std::isfinite(panel.phaseOffset) ||
            panel.densityX < 0.25f || panel.densityX > 4.0f ||
            panel.densityY < 0.25f || panel.densityY > 4.0f ||
            panel.phaseOffset < -1.0f || panel.phaseOffset > 1.0f)
            return false;
        output += "{\"enabled\":";
        output += panel.enabled ? "true" : "false";
        output += ",\"motionPattern\":";
        AppendInt(output, static_cast<int>(panel.motionPattern));
        output += ",\"direction\":";
        AppendInt(output, static_cast<int>(panel.direction));
        output += ",\"densityMode\":";
        AppendInt(output, static_cast<int>(panel.densityMode));
        output += ",\"densityX\":";
        if (!AppendFloat(output, panel.densityX)) return false;
        output += ",\"densityY\":";
        if (!AppendFloat(output, panel.densityY)) return false;
        output += ",\"phaseOffset\":";
        if (!AppendFloat(output, panel.phaseOffset)) return false;
        output += "}";
    }
    output += "]";

    output += ",\"secondaryRed\":";
    AppendUInt(output, effect.secondaryRed);
    output += ",\"secondaryGreen\":";
    AppendUInt(output, effect.secondaryGreen);
    output += ",\"secondaryBlue\":";
    AppendUInt(output, effect.secondaryBlue);
    output += ",\"tertiaryRed\":";
    AppendUInt(output, effect.tertiaryRed);
    output += ",\"tertiaryGreen\":";
    AppendUInt(output, effect.tertiaryGreen);
    output += ",\"tertiaryBlue\":";
    AppendUInt(output, effect.tertiaryBlue);
    output += ",\"colorDirection\":";
    AppendInt(output, static_cast<int>(effect.colorDirection));
    output += ",\"colorSpread\":";
    if (!AppendFloat(output, effect.colorSpread)) return false;
    output += ",\"eyeShapeVersion\":2";
    output += ",\"eyeShape\":";
    AppendInt(output, static_cast<int>(effect.eyeShape));
    output += ",\"eyeMotionVersion\":2";
    output += ",\"eyeMotion\":";
    AppendInt(output, static_cast<int>(effect.eyeMotion));
    output += ",\"eyeMotionPlayback\":";
    AppendInt(output, static_cast<int>(effect.eyeMotionPlayback));
    output += ",\"eyeMotionIntervalMs\":";
    AppendUInt(output, effect.eyeMotionIntervalMs);
    output += ",\"eyeBlinkIntervalMs\":";
    AppendUInt(output, effect.eyeBlinkIntervalMs);
    output += ",\"eyeTexturePlayback\":";
    AppendInt(output, static_cast<int>(effect.eyeTexturePlayback));
    output += ",\"eyeTextureIndex\":";
    AppendUInt(output, effect.eyeTextureIndex);
    output += ",\"eyeTextureIntervalMs\":";
    AppendUInt(output, effect.eyeTextureIntervalMs);
    output += ",\"eyeTextures\":[";
    for (size_t i = 0; i < effect.eyeTextures.size(); ++i) {
        if (i) output += ",";
        const EyeTextureDesign& design = effect.eyeTextures[i];
        output += "{\"name\":";
        if (!AppendJsonString(output, design.name)) return false;
        output += ",\"textureName\":";
        if (!AppendJsonString(output, design.textureName)) return false;
        output += ",\"mappingConfigured\":";
        output += design.mappingConfigured ? "true" : "false";
        output += ",\"irisCenterX\":";
        if (!AppendFloat(output, design.irisCenterX)) return false;
        output += ",\"irisCenterY\":";
        if (!AppendFloat(output, design.irisCenterY)) return false;
        output += ",\"irisRadiusX\":";
        if (!AppendFloat(output, design.irisRadiusX)) return false;
        output += ",\"irisRadiusY\":";
        if (!AppendFloat(output, design.irisRadiusY)) return false;
        output += ",\"pupilCenterX\":";
        if (!AppendFloat(output, design.pupilCenterX)) return false;
        output += ",\"pupilCenterY\":";
        if (!AppendFloat(output, design.pupilCenterY)) return false;
        output += ",\"pupilRadiusX\":";
        if (!AppendFloat(output, design.pupilRadiusX)) return false;
        output += ",\"pupilRadiusY\":";
        if (!AppendFloat(output, design.pupilRadiusY)) return false;
        output += "}";
    }
    output += "],\"eyeTextureSequence\":[";
    for (size_t i = 0; i < effect.eyeTextureSequence.size(); ++i) {
        if (i) output += ",";
        AppendUInt(output, effect.eyeTextureSequence[i]);
    }
    output += "]";
    output += ",\"eyeIrisRed\":";
    AppendUInt(output, effect.eyeIrisRed);
    output += ",\"eyeIrisGreen\":";
    AppendUInt(output, effect.eyeIrisGreen);
    output += ",\"eyeIrisBlue\":";
    AppendUInt(output, effect.eyeIrisBlue);
    output += ",\"eyePupilRed\":";
    AppendUInt(output, effect.eyePupilRed);
    output += ",\"eyePupilGreen\":";
    AppendUInt(output, effect.eyePupilGreen);
    output += ",\"eyePupilBlue\":";
    AppendUInt(output, effect.eyePupilBlue);
    output += ",\"eyeTextureIrisCenterX\":";
    if (!AppendFloat(output, effect.eyeTextureIrisCenterX)) return false;
    output += ",\"eyeTextureIrisCenterY\":";
    if (!AppendFloat(output, effect.eyeTextureIrisCenterY)) return false;
    output += ",\"eyeTextureIrisRadiusX\":";
    if (!AppendFloat(output, effect.eyeTextureIrisRadiusX)) return false;
    output += ",\"eyeTextureIrisRadiusY\":";
    if (!AppendFloat(output, effect.eyeTextureIrisRadiusY)) return false;
    output += ",\"eyeTexturePupilCenterX\":";
    if (!AppendFloat(output, effect.eyeTexturePupilCenterX)) return false;
    output += ",\"eyeTexturePupilCenterY\":";
    if (!AppendFloat(output, effect.eyeTexturePupilCenterY)) return false;
    output += ",\"eyeTexturePupilRadiusX\":";
    if (!AppendFloat(output, effect.eyeTexturePupilRadiusX)) return false;
    output += ",\"eyeTexturePupilRadiusY\":";
    if (!AppendFloat(output, effect.eyeTexturePupilRadiusY)) return false;

    output += ",\"speed\":";
    if (!AppendFloat(output, effect.speed)) return false;
    output += ",\"durationMs\":";
    AppendUInt(output, effect.durationMs);

    output += ",\"text\":";
    if (!AppendJsonString(output, effect.text)) return false;

    output += ",\"textSpacing\":";
    if (!AppendFloat(output, effect.textSpacing)) return false;
    output += ",\"textScale\":";
    if (!AppendFloat(output, effect.textScale)) return false;
    output += ",\"fontName\":";
    if (!AppendJsonString(output, effect.fontName)) return false;
    output += ",\"textureName\":";
    if (!AppendJsonString(output, effect.textureName)) return false;
    output += ",\"textMotion\":";
    AppendInt(output, static_cast<int>(effect.textMotion));

    output += ",\"matrix\":";

    if (!AppendMatrix(output, effect.matrix))
        return false;
    output += ",\"transparentMatrixBackground\":";
    output += effect.transparentMatrixBackground ? "true" : "false";

    output += "}";
    return true;
}

static bool AppendPlacement(
    std::string& output,
    const LedPlacement& placement
) {
    if (static_cast<int>(placement.zone) < 0 ||
        static_cast<int>(placement.zone) >
            static_cast<int>(LedZone::Roof)) {
        return false;
    }

    output += "{\"id\":";
    AppendUInt(output, placement.id);

    output += ",\"zone\":";
    AppendInt(output, static_cast<int>(placement.zone));
    output += ",\"enabled\":";
    output += placement.enabled ? "true" : "false";
    output += ",\"animationMode\":";
    AppendUInt(output, placement.animationMode);
    output += ",\"animationIntervalMs\":";
    AppendUInt(output, placement.animationIntervalMs);
    output += ",\"activeEffectIndex\":";
    AppendUInt(output, placement.activeEffectIndex);

    output += ",\"transform\":";

    if (!AppendTransform(output, placement.transform))
        return false;

    output += ",\"effects\":[";

    for (size_t i = 0; i < placement.effects.size(); ++i) {
        if (i > 0)
            output += ",";

        if (!AppendEffect(output, placement.effects[i]))
            return false;
    }

    output += "]}";
    return true;
}

static bool AppendSequenceStep(
    std::string& output,
    const EffectSequenceStep& step
) {
    output += "{\"placementId\":";
    AppendUInt(output, step.placementId);

    output += ",\"effectIndex\":";
    AppendUInt(output, step.effectIndex);

    output += ",\"durationMs\":";
    AppendUInt(output, step.durationMs);

    output += "}";
    return true;
}

static bool AppendSequence(
    std::string& output,
    const EffectSequence& sequence
) {
    output += "{\"enabled\":";
    output += sequence.enabled ? "true" : "false";

    output += ",\"loop\":";
    output += sequence.loop ? "true" : "false";

    output += ",\"transitionMode\":";
    AppendInt(output, static_cast<int>(sequence.transitionMode));
    output += ",\"transitionDirection\":";
    AppendInt(output, static_cast<int>(sequence.transitionDirection));

    output += ",\"steps\":[";

    for (size_t i = 0; i < sequence.steps.size(); ++i) {
        if (i > 0)
            output += ",";

        if (!AppendSequenceStep(output, sequence.steps[i]))
            return false;
    }

    output += "]}";
    return true;
}

} // namespace

bool Serialize(
    const VehicleProfile& profile,
    std::string& json
) {
    json.clear();

    if (!ValidateProfileReferences(profile))
        return false;

    json += "{\"modelId\":";
    AppendInt(json, profile.modelId);

    json += ",\"enabled\":";
    json += profile.enabled ? "true" : "false";

    json += ",\"nativeLights\":{\"scanMode\":";
    AppendInt(json, static_cast<int>(profile.nativeLights.scanMode));
    json += ",\"highBeamEnabled\":";
    json += profile.nativeLights.highBeamEnabled ? "true" : "false";
    json += ",\"highBeamRange\":";
    if (!AppendFloat(json, profile.nativeLights.highBeamRange)) return false;
    json += ",\"highBeamWidthDegrees\":";
    if (!AppendFloat(json, profile.nativeLights.highBeamWidthDegrees)) return false;
    json += ",\"highBeamIntensity\":";
    if (!AppendFloat(json, profile.nativeLights.highBeamIntensity)) return false;
    json += ",\"rgbHeadlightsEnabled\":";
    json += profile.nativeLights.rgbHeadlightsEnabled ? "true" : "false";
    json += ",\"rgbTaillightsEnabled\":";
    json += profile.nativeLights.rgbTaillightsEnabled ? "true" : "false";
    json += ",\"frontAxisPositive\":";
    json += profile.nativeLights.frontAxisPositive ? "true" : "false";
    json += ",\"rgbFlowSpeed\":";
    if (!AppendFloat(json, profile.nativeLights.rgbFlowSpeed)) return false;
    json += ",\"rgbFlowIntensity\":";
    if (!AppendFloat(json, profile.nativeLights.rgbFlowIntensity)) return false;
    json += ",\"rgbBlendMode\":";
    AppendInt(json, static_cast<int>(profile.nativeLights.blendMode));
    json += "}";

    json += ",\"placements\":[";

    for (size_t i = 0; i < profile.placements.size(); ++i) {
        if (i > 0)
            json += ",";

        if (!AppendPlacement(json, profile.placements[i])) {
            json.clear();
            return false;
        }

        if (json.size() > MAX_JSON_SIZE) {
            json.clear();
            return false;
        }
    }

    json += "],\"sequences\":[";

    for (size_t i = 0; i < profile.sequences.size(); ++i) {
        if (i > 0)
            json += ",";

        if (!AppendSequence(json, profile.sequences[i])) {
            json.clear();
            return false;
        }

        if (json.size() > MAX_JSON_SIZE) {
            json.clear();
            return false;
        }
    }

    json += "]}";

    if (json.size() > MAX_JSON_SIZE) {
        json.clear();
        return false;
    }

    return true;
}

bool Deserialize(
    const std::string& json,
    VehicleProfile& profile
) {
    if (json.empty() || json.size() > MAX_JSON_SIZE)
        return false;

    JsonParser parser(json);
    JsonValue root;

    if (!parser.Parse(root) ||
        root.type != JsonValue::Object) {
        return false;
    }

    VehicleProfile result;

    if (!ReadInt(root, "modelId", result.modelId) ||
        !ReadBool(root, "enabled", result.enabled)) {
        return false;
    }

    const JsonValue* nativeLights = Find(root, "nativeLights");
    if (nativeLights != nullptr) {
        int scanMode = static_cast<int>(result.nativeLights.scanMode);
        int rgbBlendMode = static_cast<int>(result.nativeLights.blendMode);
        if (nativeLights->type != JsonValue::Object ||
            (Find(*nativeLights, "scanMode") != nullptr &&
             (!ReadInt(*nativeLights, "scanMode", scanMode) ||
              scanMode < 0 || scanMode > static_cast<int>(NativeLightScanMode::TextureName))) ||
            !ReadBool(*nativeLights, "highBeamEnabled", result.nativeLights.highBeamEnabled) ||
            !ReadOptionalFloat(*nativeLights, "highBeamRange", result.nativeLights.highBeamRange) ||
            !ReadOptionalFloat(*nativeLights, "highBeamWidthDegrees", result.nativeLights.highBeamWidthDegrees) ||
            !ReadOptionalFloat(*nativeLights, "highBeamIntensity", result.nativeLights.highBeamIntensity) ||
            !ReadBool(*nativeLights, "rgbHeadlightsEnabled", result.nativeLights.rgbHeadlightsEnabled) ||
            !ReadBool(*nativeLights, "rgbTaillightsEnabled", result.nativeLights.rgbTaillightsEnabled) ||
            (Find(*nativeLights, "frontAxisPositive") != nullptr &&
             !ReadBool(*nativeLights, "frontAxisPositive", result.nativeLights.frontAxisPositive)) ||
            !ReadOptionalFloat(*nativeLights, "rgbFlowSpeed", result.nativeLights.rgbFlowSpeed) ||
            !ReadOptionalFloat(*nativeLights, "rgbFlowIntensity", result.nativeLights.rgbFlowIntensity) ||
            (Find(*nativeLights, "rgbBlendMode") != nullptr &&
             (!ReadInt(*nativeLights, "rgbBlendMode", rgbBlendMode) ||
              rgbBlendMode < 0 || rgbBlendMode > static_cast<int>(LedLayerBlendMode::Additive)))) {
            return false;
        }
        result.nativeLights.scanMode = static_cast<NativeLightScanMode>(scanMode);
        result.nativeLights.blendMode = static_cast<LedLayerBlendMode>(rgbBlendMode);
        if (!std::isfinite(result.nativeLights.highBeamRange) ||
            !std::isfinite(result.nativeLights.highBeamWidthDegrees) ||
            !std::isfinite(result.nativeLights.highBeamIntensity) ||
            result.nativeLights.highBeamRange < 10.0f ||
            result.nativeLights.highBeamRange > 500.0f ||
            result.nativeLights.highBeamWidthDegrees < 5.0f ||
            result.nativeLights.highBeamWidthDegrees > 45.0f ||
            result.nativeLights.highBeamIntensity < 0.1f ||
            result.nativeLights.highBeamIntensity > 4.0f ||
            result.nativeLights.rgbFlowSpeed < 0.1f ||
            result.nativeLights.rgbFlowSpeed > 4.0f ||
            result.nativeLights.rgbFlowIntensity < 0.1f ||
            result.nativeLights.rgbFlowIntensity > 2.0f) {
            return false;
        }
    }

    const JsonValue* placements = Find(root, "placements");

    if (placements != nullptr) {
        if (placements->type != JsonValue::Array)
            return false;

        for (const JsonValue& item : placements->array) {
            LedPlacement placement;

            if (!ReadPlacement(item, placement))
                return false;

            result.placements.push_back(placement);
        }
    }

    const JsonValue* sequences = Find(root, "sequences");

    if (sequences != nullptr) {
        if (sequences->type != JsonValue::Array)
            return false;

        for (const JsonValue& item : sequences->array) {
            EffectSequence sequence;

            if (!ReadSequence(item, sequence))
                return false;

            result.sequences.push_back(sequence);
        }
    }

    if (!ValidateProfileReferences(result))
        return false;

    profile = result;
    return true;
}

} // namespace ProfileJson
} // namespace VLS
