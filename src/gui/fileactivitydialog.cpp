/*
 * Copyright (C) by Felix Weilbach <felix.weilbach@nextcloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */
#include <theme.h>

#include "activityjob.h"
#include "fileactivitydialog.h"
#include "fileactivitymodel.h"
#include "ui_fileactivitydialog.h"

#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QSpinBox>
#include <QPainter>
#include <QDebug>
#include <QLoggingCategory>
#include <QAbstractItemModel>
#include <QPalette>


Q_LOGGING_CATEGORY(lcFileActivity, "nextcloud.gui.fileactivity", QtInfoMsg)

namespace OCC {

class FileActivityDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    FileActivityDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
        _iconSize = static_cast<int>(static_cast<double>(_iconSize) * Theme::pixelRatio());
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        painter->save();
        painter->setClipping(true);
        painter->setClipRect(opt.rect);
        painter->setFont(opt.font);

        drawBackground(opt, painter);
        drawBottomLine(opt, index, painter);
        const auto contentRect = opt.rect.adjusted(_margins.left(), _margins.top(), -_margins.right(), -_margins.bottom());
        const auto item = qvariant_cast<FileActivity>(index.data());
        drawMessageIcon(contentRect, item, painter);
        const auto timestampRect = drawTimestamp(opt, contentRect, item, painter);
        drawMessage(opt, timestampRect, item, painter);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const auto textHeight = timestampBox(opt, index).height()
            + _spacingVertical + messageBox(opt, index).height();
        const auto iconHeight = _iconSize;
        const auto fullHeight = textHeight > iconHeight ? textHeight : iconHeight;

        return { opt.rect.width(), _margins.top() + fullHeight + _margins.bottom() };
    }

private:
    QString timestampReadable(const QDateTime &dateTime) const
    {
        return Utility::timeAgoInWords(dateTime.toLocalTime());
    }

    QRect timestampBox(const QStyleOptionViewItem &option, const FileActivity &fileActivity) const
    {
        auto timestampFont = option.font;

        timestampFont.setPointSizeF(timestampFontPointSize(option.font));

        return QFontMetrics(timestampFont)
            .boundingRect(timestampReadable(fileActivity.timestamp()))
            .adjusted(0, 0, 1, 1);
    }

    QRect timestampBox(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return timestampBox(option, qvariant_cast<FileActivity>(index.data()));
    }

    int timestampFontPointSize(const QFont &font) const
    {
        return static_cast<int>(_timestampFontSizeRatio * static_cast<float>(font.pointSize()));
    }

    QRect messageBox(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        return messageBox(option, qvariant_cast<FileActivity>(index.data()).message());
    }

    QRect messageBox(const QStyleOptionViewItem &option, const QString &message) const
    {
        return option.fontMetrics.boundingRect(message).adjusted(0, 0, 1, 1);
    }

    QRect drawTimestamp(const QStyleOptionViewItem &opt, const QRect &contentRectangle, const OCC::FileActivity &item, QPainter *painter) const
    {
        QFont font(opt.font);
        font.setPointSize(timestampFontPointSize(opt.font));
        const auto palette = opt.palette;
        QRect timestampRect(timestampBox(opt, item));

        timestampRect.moveTo(_margins.left() + _iconSize + _spacingHorizontal, contentRectangle.top());

        painter->setFont(font);
        painter->setPen(palette.text().color());
        painter->drawText(timestampRect, Qt::TextSingleLine, timestampReadable(item.timestamp()));

        return timestampRect;
    }

    void drawBottomLine(const QStyleOptionViewItem &opt, const QModelIndex &index, QPainter *painter) const
    {
        const bool lastIndex = (index.model()->rowCount() - 1) == index.row();
        painter->setPen(opt.palette.window().color());
        painter->drawLine(lastIndex ? opt.rect.left() : _margins.left(),
            opt.rect.bottom(), lastIndex ? opt.rect.right() : opt.rect.right() - _margins.right(), opt.rect.bottom());
    }

    void drawMessageIcon(const QRect &contentRect, const FileActivity &item, QPainter *painter) const
    {
        const auto pixmap = FileActivityListModel::pixmapForActivityType(item.type(), _iconSize);
        Q_ASSERT(pixmap);
        painter->drawPixmap(contentRect.left(), contentRect.top(), *pixmap);
    }

    void drawMessage(const QStyleOptionViewItem &opt, const QRect &timestampRect, const OCC::FileActivity &item, QPainter *painter) const
    {
        QRect messageRect(messageBox(opt, item.message()));

        messageRect.moveTo(timestampRect.left(), timestampRect.bottom() + _spacingVertical);

        painter->setFont(opt.font);
        painter->setPen(opt.palette.windowText().color());
        painter->drawText(messageRect, Qt::TextSingleLine, item.message());
    }

    void drawBackground(const QStyleOptionViewItem &opt, QPainter *painter) const
    {
        painter->fillRect(opt.rect, opt.state & QStyle::State_Selected ? opt.palette.highlight().color() : opt.palette.light().color());
    }

    int _iconSize = 32;
    const QMargins _margins = { 8, 8, 8, 8 };
    const int _spacingHorizontal = 8;
    const int _spacingVertical = 4;
    const float _timestampFontSizeRatio = 0.85;
};

FileActivityDialog::FileActivityDialog(AccountPtr account, const QString &fileId, QWidget *parent)
    : QDialog(parent)
    , _ui(new Ui::FileActivityDialog)
    , _model(std::make_unique<OcsActivityJob>(account))
{
    _ui->setupUi(this);
    _ui->activityListView->setModel(_model.getActivityListModel());
    _ui->activityListView->setItemDelegate(new FileActivityDelegate(this));


    connect(&_model, &FileActivityDialogModel::showActivities, this, &FileActivityDialog::onShowActivities);
    connect(&_model, &FileActivityDialogModel::hideActivities, this, &FileActivityDialog::onHideActivities);
    connect(&_model, &FileActivityDialogModel::showError, this, &FileActivityDialog::onShowError);
    connect(&_model, &FileActivityDialogModel::hideError, this, &FileActivityDialog::onHideError);
    connect(&_model, &FileActivityDialogModel::showProgress, this, &FileActivityDialog::onShowProgress);
    connect(&_model, &FileActivityDialogModel::hideProgress, this, &FileActivityDialog::onHideProgress);
    _model.start(fileId);

    setWindowTitle(tr("File activity"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    _ui->progressIndicator->setColor(palette().color(QPalette::WindowText));
}

void FileActivityDialog::onShowActivities()
{
    _ui->activityListView->show();
}

void FileActivityDialog::onHideActivities()
{
    _ui->activityListView->hide();
}

void FileActivityDialog::onShowError(const QString &message)
{
    _ui->errorLabel->setText(message);
    _ui->errorLabel->show();
}

void FileActivityDialog::onHideError()
{
    _ui->errorLabel->hide();
}

void FileActivityDialog::onShowProgress()
{
    _ui->progressIndicator->show();
    _ui->progressIndicator->startAnimation();
}

void FileActivityDialog::onHideProgress()
{
    _ui->progressIndicator->hide();
    _ui->progressIndicator->stopAnimation();
}
}

#include "fileactivitydialog.moc"
