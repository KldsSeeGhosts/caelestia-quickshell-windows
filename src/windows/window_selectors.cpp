#include "window_selectors.hpp"

#include <qfileinfo.h>
#include <qloggingcategory.h>

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logSelectors, "quickshell.windows.selectors", QtWarningMsg);
}

WindowSelectors::PatternSet WindowSelectors::compile(
    const QStringList& patterns,
    ScreenSelectors screens,
    qsizetype& configured
) {
	auto compiled = PatternSet();
	configured = 0;

	for (qsizetype index = 0; index < patterns.size(); ++index) {
		auto pattern = patterns.at(index).trimmed();
		auto* target = &compiled.windows;
		auto screenRule = false;
		for (const auto& prefix:
		     {QStringLiteral("title:"),
		      QStringLiteral("exe:"),
		      QStringLiteral("class:"),
		      QStringLiteral("screen:")})
		{
			if (!pattern.startsWith(prefix, Qt::CaseInsensitive)) continue;
			if (prefix == QStringLiteral("title:")) target = &compiled.titles;
			else if (prefix == QStringLiteral("exe:")) target = &compiled.executables;
			else if (prefix == QStringLiteral("class:")) target = &compiled.classes;
			else {
				target = &compiled.screens;
				screenRule = true;
			}
			pattern = pattern.mid(prefix.size()).trimmed();
			break;
		}
		if (pattern.isEmpty()) continue;
		configured++;

		if (screenRule && screens == ScreenSelectors::Reject) {
			// The index is stable and safe; the selector text may carry a user
			// title, executable name or path and must not reach a log.
			qCWarning(logSelectors).noquote()
			    << QStringLiteral("selector %1: screen selectors are not supported").arg(index + 1);
			continue;
		}

		auto expression = QRegularExpression(
		    QRegularExpression::wildcardToRegularExpression(
		        pattern,
		        QRegularExpression::NonPathWildcardConversion
		    ),
		    QRegularExpression::CaseInsensitiveOption
		);

		if (!expression.isValid()) {
			qCWarning(logSelectors).noquote()
			    << QStringLiteral("selector %1: invalid pattern").arg(index + 1);
			continue;
		}

		// An app can show an empty title for a moment while it changes state. A
		// title rule that matches "" would then catch every such window.
		if (target == &compiled.titles && expression.match(QString()).hasMatch()) {
			qCWarning(logSelectors).noquote()
			    << QStringLiteral("selector %1: title pattern matches an empty title").arg(index + 1);
			continue;
		}

		target->append(expression);
	}

	return compiled;
}

bool WindowSelectors::matchTitles(const QList<QRegularExpression>& titles, const QString& title) {
	if (title.isEmpty()) return false;

	for (const auto& pattern: titles) {
		if (pattern.match(title).hasMatch()) return true;
	}

	return false;
}

bool WindowSelectors::match(
    const PatternSet& patterns,
    const QString& appId,
    const QString& exePath,
    const QString& title,
    const QString& className,
    const QString& screenName
) {
	if (WindowSelectors::matchTitles(patterns.titles, title)) return true;

	auto exe = QFileInfo(exePath).fileName();
	auto base = exe;
	if (base.endsWith(".exe", Qt::CaseInsensitive)) base.chop(4);

	for (const auto& pattern: patterns.executables) {
		if (pattern.match(exe).hasMatch() || pattern.match(base).hasMatch()) return true;
	}
	for (const auto& pattern: patterns.classes) {
		if (pattern.match(className).hasMatch()) return true;
	}
	for (const auto& pattern: patterns.screens) {
		if (pattern.match(screenName).hasMatch()) return true;
	}

	auto candidates = QStringList {appId, exe, base, className};

	for (const auto& pattern: patterns.windows) {
		for (const auto& candidate: candidates) {
			if (!candidate.isEmpty() && pattern.match(candidate).hasMatch()) return true;
		}
	}

	return false;
}

void WindowSelectors::setPatterns(const QStringList& patterns, ScreenSelectors screens) {
	this->mPatterns = patterns;
	this->rules = WindowSelectors::compile(patterns, screens, this->mConfigured);
}

qsizetype WindowSelectors::compiledCount() const {
	return this->rules.windows.size() + this->rules.titles.size() + this->rules.executables.size()
	     + this->rules.classes.size() + this->rules.screens.size();
}

bool WindowSelectors::matchesTitle(const QString& title) const {
	return WindowSelectors::matchTitles(this->rules.titles, title);
}

bool WindowSelectors::isEmpty() const {
	return this->rules.windows.isEmpty() && this->rules.titles.isEmpty()
	    && this->rules.executables.isEmpty() && this->rules.classes.isEmpty()
	    && this->rules.screens.isEmpty();
}

bool WindowSelectors::hasTitleRules() const { return !this->rules.titles.isEmpty(); }

bool WindowSelectors::matches(
    const QString& appId,
    const QString& exePath,
    const QString& title,
    const QString& className,
    const QString& screenName
) const {
	return WindowSelectors::match(this->rules, appId, exePath, title, className, screenName);
}

} // namespace qs::windows
