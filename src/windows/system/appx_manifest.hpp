#pragma once

#include <qstringlist.h>
#include <qxmlstream.h>

namespace qs::windows::sys {

struct AppxManifestInfo {
	bool applicationFound = false;
	bool allowElevation = false;
	QStringList desktopExecutionAliases;
};

namespace detail {

inline bool isDesktopApp(const QXmlStreamAttributes& attributes) {
	for (const auto& attribute: attributes) {
		if (attribute.name() == u"EntryPoint" && attribute.value() == u"Windows.FullTrustApplication")
			return true;
		if (attribute.name() == u"RuntimeBehavior"
		    && (attribute.value() == u"packagedClassicApp" || attribute.value() == u"win32App"))
			return true;
	}
	return false;
}

inline void readExecutionAliases(QXmlStreamReader& xml, bool desktop, QStringList& aliases) {
	while (xml.readNextStartElement()) {
		if (xml.name() == u"Extension"
		    && xml.attributes().value(u"Category") == u"windows.appExecutionAlias"
		    && (desktop || isDesktopApp(xml.attributes())))
		{
			while (xml.readNextStartElement()) {
				if (xml.name() == u"AppExecutionAlias") {
					while (xml.readNextStartElement()) {
						if (xml.name() == u"ExecutionAlias") {
							const auto alias = xml.attributes().value(u"Alias");
							if (alias.size() > 4 && alias.endsWith(u".exe", Qt::CaseInsensitive)
							    && alias.indexOf(u'/') < 0 && alias.indexOf(u'\\') < 0
							    && alias.indexOf(u':') < 0 && alias.indexOf(u'*') < 0
							    && alias.indexOf(u'?') < 0)
							{
								aliases.append(alias.toString());
							}
						}
						xml.skipCurrentElement();
					}
				} else {
					xml.skipCurrentElement();
				}
			}
		} else {
			xml.skipCurrentElement();
		}
	}
}

} // namespace detail

inline AppxManifestInfo readAppxManifest(QIODevice* manifest, QStringView applicationId) {
	auto xml = QXmlStreamReader(manifest);
	if (applicationId.isEmpty() || !xml.readNextStartElement() || xml.name() != u"Package") return {};

	auto info = AppxManifestInfo();
	while (xml.readNextStartElement()) {
		if (xml.name() == u"Capabilities") {
			while (xml.readNextStartElement()) {
				if (xml.name() == u"Capability" && xml.attributes().value(u"Name") == u"allowElevation")
					info.allowElevation = true;
				xml.skipCurrentElement();
			}
		} else if (xml.name() == u"Applications") {
			while (xml.readNextStartElement()) {
				if (xml.name() != u"Application" || xml.attributes().value(u"Id") != applicationId) {
					xml.skipCurrentElement();
					continue;
				}
				info.applicationFound = true;
				const auto desktop = detail::isDesktopApp(xml.attributes());
				while (xml.readNextStartElement()) {
					if (xml.name() == u"Extensions") {
						detail::readExecutionAliases(xml, desktop, info.desktopExecutionAliases);
					} else {
						xml.skipCurrentElement();
					}
				}
			}
		} else {
			xml.skipCurrentElement();
		}
	}
	while (!xml.atEnd()) xml.readNext();
	return xml.hasError() || !info.applicationFound ? AppxManifestInfo() : info;
}

} // namespace qs::windows::sys
