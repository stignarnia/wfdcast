// SPDX-FileCopyrightText: 2026 stignarnia
// SPDX-License-Identifier: MIT
#pragma once

#include <KConfigGroup>
#include <QQmlPropertyMap>

// Every tunable of the app, persisted in ~/.config/wfdcastrc and exposed to QML
// as `settings.<key>` (read/write). Nothing the user might want to change is
// hardcoded elsewhere: code reads values from here.
class Settings : public QQmlPropertyMap
{
    Q_OBJECT
public:
    explicit Settings(QObject *parent = nullptr);

    QVariant get(const QString &key) const;
    int getInt(const QString &key) const;
    QString getString(const QString &key) const;
    bool getBool(const QString &key) const;
    void put(const QString &key, const QVariant &value);

    Q_INVOKABLE void resetToDefaults();
    Q_INVOKABLE QVariant defaultValue(const QString &key) const;

protected:
    QVariant updateValue(const QString &key, const QVariant &input) override;

private:
    void save(const QString &key, const QVariant &value);
    QVariantMap m_defaults;
    KConfigGroup m_group;
};
