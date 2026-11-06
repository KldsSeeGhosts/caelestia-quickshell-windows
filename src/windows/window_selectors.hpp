#pragma once

#include <qlist.h>
#include <qregularexpression.h>
#include <qstring.h>
#include <qstringlist.h>

namespace qs::windows {

// Whether the caller can use `screen:` selectors. A shell-surface rule is not
// scoped to a monitor, and an empty screen name would make `screen:*` match
// every window, so that list rejects them.
enum class ScreenSelectors { Allow, Reject };

// Shared typed wildcard selectors for window rules. The dialect is the same
// wherever the shell accepts selectors (tiling exclusions and the optional
// shell-surface list):
//
//   title:<pattern>   window title
//   exe:<pattern>     executable file name or basename
//   class:<pattern>   Win32 window class
//   screen:<pattern>  screen name (only where ScreenSelectors::Allow)
//   <pattern>         app id, executable file name/basename or class
//
// Matching is case-insensitive and whole-value; `*`, `?` and character classes
// are wildcards. An empty or whitespace-only entry is ignored. A selector that
// does not compile, and a `title:` selector that matches the empty string (a
// transient empty title must never match a rule), is dropped; only its index
// and a safe category are logged, never the pattern text.
//
// configuredCount() counts the entries the user wrote (non-blank after the
// prefix); compiledCount() counts the ones that survived validation. A caller
// that uses the list as an allowlist must treat "configured but none compiled"
// as an allowlist that matches nothing, never as no allowlist.
class WindowSelectors {
public:
	void setPatterns(
	    const QStringList& patterns,
	    ScreenSelectors screens = ScreenSelectors::Allow
	);
	[[nodiscard]] const QStringList& patterns() const { return this->mPatterns; }
	[[nodiscard]] bool isEmpty() const;
	[[nodiscard]] bool hasTitleRules() const;
	[[nodiscard]] qsizetype configuredCount() const { return this->mConfigured; }
	[[nodiscard]] qsizetype compiledCount() const;

	// Whether a `title:` selector matches `title`. Only the title rules are
	// evaluated; an empty title never matches.
	[[nodiscard]] bool matchesTitle(const QString& title) const;

	[[nodiscard]] bool matches(
	    const QString& appId,
	    const QString& exePath,
	    const QString& title,
	    const QString& className,
	    const QString& screenName
	) const;

private:
	struct PatternSet {
		QList<QRegularExpression> windows;
		QList<QRegularExpression> titles;
		QList<QRegularExpression> executables;
		QList<QRegularExpression> classes;
		QList<QRegularExpression> screens;
	};

	[[nodiscard]] static PatternSet compile(
	    const QStringList& patterns,
	    ScreenSelectors screens,
	    qsizetype& configured
	);
	[[nodiscard]] static bool matchTitles(const QList<QRegularExpression>& titles, const QString& title);
	[[nodiscard]] static bool match(
	    const PatternSet& patterns,
	    const QString& appId,
	    const QString& exePath,
	    const QString& title,
	    const QString& className,
	    const QString& screenName
	);

	QStringList mPatterns;
	qsizetype mConfigured = 0;
	PatternSet rules;
};

} // namespace qs::windows
