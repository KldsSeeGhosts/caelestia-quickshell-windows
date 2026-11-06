#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstringlist.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <qvariant.h>

namespace qs::windows {

// Profile-supplied extra shell-surface selectors. The generic classifier covers
// Windows' own shell UI; this list is the explicit fallback for third-party
// shell windows (for example an activatable, taskbar-hidden flyout). It is
// empty by default and uses the same typed wildcard dialect as the tiling
// allowlist, without `screen:`. Non-string config elements are ignored and
// counted.
class ShellSurfaces: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(QVariant extra READ extra WRITE setExtra NOTIFY extraChanged);
	// clang-format on

public:
	explicit ShellSurfaces(QObject* parent = nullptr);

	[[nodiscard]] QVariant extra() const;
	void setExtra(const QVariant& extra);

signals:
	void extraChanged();

private:
	QStringList mExtra;
};

} // namespace qs::windows
