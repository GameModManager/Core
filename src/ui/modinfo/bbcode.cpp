#include "ui/modinfo/bbcode.h"

#include <QRegularExpression>
#include <QStack>

namespace ui {

namespace {

struct Tag {
    QString name;
    QString arg;
};

// Escape text for HTML. Also converts newlines (Nexus BBCode uses \n for
// line breaks outside [code]).
QString escape(const QString& text, bool code = false) {
    QString out = text.toHtmlEscaped();
    if (code) return out;
    out.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return out;
}

QStringList split_tags(const QString& input) {
    static const QRegularExpression re(
        QStringLiteral("\\[([a-zA-Z0-9]+)(?:=([^\\]]*))?\\]|\\[/([a-zA-Z0-9]+)\\]"));
    QStringList parts;
    int pos = 0;
    auto it = re.globalMatch(input);
    while (it.hasNext()) {
        const auto m = it.next();
        if (m.capturedStart() > pos)
            parts << input.mid(pos, m.capturedStart() - pos);
        parts << m.captured(0);
        pos = m.capturedEnd();
    }
    if (pos < input.length()) parts << input.mid(pos);
    return parts;
}

QString wrap(const QString& name, const QString& arg, const QString& content) {
    const QString c = content;
    if (name == QStringLiteral("b")) return QStringLiteral("<b>") + c + QStringLiteral("</b>");
    if (name == QStringLiteral("i")) return QStringLiteral("<i>") + c + QStringLiteral("</i>");
    if (name == QStringLiteral("u")) return QStringLiteral("<u>") + c + QStringLiteral("</u>");
    if (name == QStringLiteral("s") || name == QStringLiteral("strike"))
        return QStringLiteral("<s>") + c + QStringLiteral("</s>");
    if (name == QStringLiteral("color"))
        return QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(arg.toHtmlEscaped(), c);
    if (name == QStringLiteral("size"))
        return QStringLiteral("<span style=\"font-size:%1px;\">%2</span>").arg(arg.toHtmlEscaped(), c);
    if (name == QStringLiteral("font"))
        return QStringLiteral("<span style=\"font-family:'%1';\">%2</span>").arg(arg.toHtmlEscaped(), c);
    if (name == QStringLiteral("url")) {
        QString href = arg.isEmpty() ? c.trimmed() : arg;
        if (!href.startsWith(QStringLiteral("http")))
            href = QStringLiteral("https://") + href;
        return QStringLiteral("<a href=\"%1\">%2</a>")
            .arg(href.toHtmlEscaped(), c);
    }
    if (name == QStringLiteral("img"))
        return QStringLiteral("<img src=\"%1\" alt=\"%2\">")
            .arg(arg.toHtmlEscaped(), c.toHtmlEscaped());
    if (name == QStringLiteral("quote"))
        return QStringLiteral("<blockquote>") + c + QStringLiteral("</blockquote>");
    if (name == QStringLiteral("code"))
        return QStringLiteral("<pre><code>") + c + QStringLiteral("</code></pre>");
    if (name == QStringLiteral("list"))
        return QStringLiteral("<ul>") + c + QStringLiteral("</ul>");
    if (name == QStringLiteral("olist") || name == QStringLiteral("list=1"))
        return QStringLiteral("<ol>") + c + QStringLiteral("</ol>");
    if (name == QStringLiteral("*"))
        return QStringLiteral("<li>") + c + QStringLiteral("</li>");
    if (name == QStringLiteral("hr")) return QStringLiteral("<hr>");
    if (name == QStringLiteral("center"))
        return QStringLiteral("<div style=\"text-align:center;\">") + c + QStringLiteral("</div>");
    if (name == QStringLiteral("spoiler"))
        return QStringLiteral("<details><summary>Spoiler</summary><div class=\"spoiler_content\">") + c + QStringLiteral("</div></details>");
    if (name == QStringLiteral("heading"))
        return QStringLiteral("<h3>") + c + QStringLiteral("</h3>");
    if (name == QStringLiteral("h1")) return QStringLiteral("<h1>") + c + QStringLiteral("</h1>");
    if (name == QStringLiteral("h2")) return QStringLiteral("<h2>") + c + QStringLiteral("</h2>");
    if (name == QStringLiteral("h3")) return QStringLiteral("<h3>") + c + QStringLiteral("</h3>");
    if (name == QStringLiteral("h4")) return QStringLiteral("<h4>") + c + QStringLiteral("</h4>");
    if (name == QStringLiteral("sub")) return QStringLiteral("<sub>") + c + QStringLiteral("</sub>");
    if (name == QStringLiteral("sup")) return QStringLiteral("<sup>") + c + QStringLiteral("</sup>");
    return c;
}

}  // namespace

QString bbcode_to_html(const QString& input) {
    if (input.isEmpty()) return {};

    const auto parts = split_tags(input);

    struct Node {
        Tag tag;
        QStringList children;
    };
    QStack<Node> stack;
    stack.push({{QStringLiteral("__root__"), {}}, {}});

    for (const auto& part : parts) {
        static const QRegularExpression open_re(
            QStringLiteral("^\\[([a-zA-Z0-9]+)(?:=([^\\]]*))?\\]$"));
        static const QRegularExpression close_re(
            QStringLiteral("^\\[/([a-zA-Z0-9]+)\\]$"));

        const auto om = open_re.match(part);
        if (om.hasMatch()) {
            const QString name = om.captured(1).toLower();
            const QString arg = om.captured(2).trimmed();
            if (name == QStringLiteral("list=1")) {
                stack.push({{QStringLiteral("olist"), {}}, {}});
            } else {
                stack.push({{name, arg}, {}});
            }
            continue;
        }

        const auto cm = close_re.match(part);
        if (cm.hasMatch()) {
            const QString name = cm.captured(1).toLower();
            // Search the stack for a matching opener (handles malformed nesting).
            int idx = -1;
            for (int i = stack.size() - 1; i > 0; --i) {
                if (stack[i].tag.name == name) { idx = i; break; }
            }
            if (idx < 0) continue;
            // Pop up to and including the opener, wrapping along the way.
            while (stack.size() - 1 > idx) {
                Node node = stack.pop();
                stack.top().children.push_back(wrap(node.tag.name, node.tag.arg,
                                                   node.children.join(QString())));
            }
            Node node = stack.pop();
            stack.top().children.push_back(wrap(node.tag.name, node.tag.arg,
                                               node.children.join(QString())));
            continue;
        }

        // Plain text.
        bool in_code = false;
        for (const auto& n : stack) {
            if (n.tag.name == QStringLiteral("code")) { in_code = true; break; }
        }
        stack.top().children.push_back(escape(part, in_code));
    }

    // Unwind any unclosed tags (already wrapped by our stack logic).
    while (stack.size() > 1) {
        Node node = stack.pop();
        stack.top().children.push_back(
            wrap(node.tag.name, node.tag.arg, node.children.join(QString())));
    }

    return stack.top().children.join(QString());
}

}  // namespace ui
