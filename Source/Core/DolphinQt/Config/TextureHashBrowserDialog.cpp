// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/TextureHashBrowserDialog.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <QAbstractItemView>
#include <QBrush>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"

#include "DolphinQt/Settings.h"

namespace
{
// Grid-view card geometry.
constexpr int GRID_THUMB_SIZE = 96;
constexpr int GRID_CARD_W = 132;
constexpr int GRID_CARD_H = 156;

QString ToHashHex(u64 hash)
{
  return QStringLiteral("%1").arg(static_cast<qulonglong>(hash), 16, 16, QLatin1Char('0'));
}

// A "list" glyph: rows of a small square marker followed by a bar. Painted in the given color so it
// adapts to the active (light/dark) theme.
QIcon MakeListViewIcon(const QColor& color)
{
  QPixmap pixmap(48, 48);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);

  constexpr int rows = 4;
  constexpr int margin = 7;
  constexpr int row_h = 5;
  constexpr int gap = (48 - 2 * margin - rows * row_h) / (rows - 1);
  for (int i = 0; i < rows; ++i)
  {
    const int y = margin + i * (row_h + gap);
    painter.drawRoundedRect(margin, y, row_h, row_h, 1, 1);                     // marker
    painter.drawRoundedRect(margin + row_h + 4, y, 48 - margin - (margin + row_h + 4), row_h, 1, 1);
  }
  return QIcon(pixmap);
}

// A "grid" glyph: a 2x2 block of rounded squares.
QIcon MakeGridViewIcon(const QColor& color)
{
  QPixmap pixmap(48, 48);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);

  constexpr int margin = 7;
  constexpr int cell_gap = 6;
  constexpr int cell = (48 - 2 * margin - cell_gap) / 2;
  for (int r = 0; r < 2; ++r)
  {
    for (int c = 0; c < 2; ++c)
    {
      const int x = margin + c * (cell + cell_gap);
      const int y = margin + r * (cell + cell_gap);
      painter.drawRoundedRect(x, y, cell, cell, 2, 2);
    }
  }
  return QIcon(pixmap);
}

std::unordered_map<u64, QString> FindTexturePreviewPaths(const std::vector<u64>& hashes)
{
  std::unordered_map<u64, QString> result;
  if (hashes.empty())
    return result;

  std::unordered_set<u64> remaining(hashes.begin(), hashes.end());
  std::unordered_map<u64, QString> patterns;
  patterns.reserve(remaining.size());
  for (u64 hash : remaining)
    patterns.emplace(hash, QStringLiteral("_%1_").arg(ToHashHex(hash)));

  const QString dump_root = QString::fromStdString(File::GetUserPath(D_DUMPTEXTURES_IDX));
  QStringList roots;
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (!game_id.empty())
    roots.push_back(QDir::cleanPath(dump_root + QString::fromStdString(game_id)));
  roots.push_back(QDir::cleanPath(dump_root));

  for (const QString& root : roots)
  {
    if (!QDir(root).exists())
      continue;

    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && !remaining.empty())
    {
      const QString path = it.next();
      const QFileInfo info(path);
      const QString suffix = info.suffix().toLower();
      if (suffix != QStringLiteral("png") && suffix != QStringLiteral("jpg") &&
          suffix != QStringLiteral("jpeg") && suffix != QStringLiteral("bmp") &&
          suffix != QStringLiteral("tga"))
      {
        continue;
      }

      const QString filename = info.fileName().toLower();
      for (auto iter = remaining.begin(); iter != remaining.end();)
      {
        const u64 hash = *iter;
        if (filename.contains(patterns[hash]))
        {
          result.emplace(hash, path);
          iter = remaining.erase(iter);
        }
        else
        {
          ++iter;
        }
      }
    }

    if (remaining.empty())
      break;
  }

  return result;
}

QPixmap LoadPreviewPixmapFresh(const QString& path)
{
  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QImage image = reader.read();
  if (image.isNull())
    return {};
  return QPixmap::fromImage(image);
}
}  // namespace

QDialog* ShowTextureHashBrowserDialog(QWidget* parent, const TextureHashBrowserConfig& config)
{
  auto* dlg = new QDialog(parent);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(config.title);
  dlg->setMinimumSize(620, 320);

  auto* info = new QLabel;
  info->setWordWrap(true);

  auto* continuous_scan_check = new QCheckBox(QObject::tr("Continuous Scan"));
  continuous_scan_check->setToolTip(
      QObject::tr("Continuously refresh while this window is open.\n"
                   "Textures seen during scan stay visible in gray."));

  // Optional Preview-mode selector (Skip/Pink): live-previews checked textures in-game.
  QComboBox* preview_mode_combo = nullptr;
  if (config.preview_mode_changed)
  {
    preview_mode_combo = new QComboBox;
    preview_mode_combo->addItem(QObject::tr("Preview: Skip"), false);
    preview_mode_combo->addItem(QObject::tr("Preview: Pink"), true);
    preview_mode_combo->setToolTip(
        QObject::tr("How checked textures are previewed in-game while this window is open.\n"
                    "Skip = hide the draws; Pink = highlight them in magenta."));
    QObject::connect(preview_mode_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), dlg,
                     [preview_mode_combo, config](int) {
                       config.preview_mode_changed(preview_mode_combo->currentData().toBool());
                     });
  }

  auto* scan_timer = new QTimer(dlg);
  scan_timer->setInterval(250);

  auto* tree = new QTreeWidget;
  tree->setHeaderLabels({QObject::tr("Use"), QObject::tr("Texture Hash"), QObject::tr("Name"),
                         QObject::tr("Preview")});
  tree->setRootIsDecorated(false);
  tree->setSelectionMode(QAbstractItemView::NoSelection);
  tree->header()->setStretchLastSection(true);
  tree->setIconSize(QSize(64, 64));

  // Grid view: thumbnail-focused cells that reflow to fill the available width. Clicking a
  // thumbnail toggles its selection (multi-select, no modifier needed); selected cells are
  // highlighted, and that selection is the set of chosen texture hashes.
  auto* grid = new QListWidget;
  grid->setViewMode(QListView::IconMode);
  grid->setResizeMode(QListView::Adjust);
  grid->setMovement(QListView::Static);
  grid->setSelectionMode(QAbstractItemView::MultiSelection);
  grid->setUniformItemSizes(true);
  grid->setSpacing(8);
  grid->setWordWrap(true);
  grid->setIconSize(QSize(GRID_THUMB_SIZE, GRID_THUMB_SIZE));
  grid->setGridSize(QSize(GRID_CARD_W, GRID_CARD_H));
  grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

  auto* view_stack = new QStackedWidget;
  view_stack->addWidget(tree);  // index 0 = List View
  view_stack->addWidget(grid);  // index 1 = Grid View

  // Segmented List/Grid toggle: two icon buttons, exactly one checked at a time.
  const QColor icon_color = dlg->palette().color(QPalette::WindowText);

  auto* list_button = new QToolButton;
  list_button->setCheckable(true);
  list_button->setIcon(MakeListViewIcon(icon_color));
  list_button->setIconSize(QSize(22, 22));
  list_button->setToolTip(QObject::tr("List View"));

  auto* grid_button = new QToolButton;
  grid_button->setCheckable(true);
  grid_button->setIcon(MakeGridViewIcon(icon_color));
  grid_button->setIconSize(QSize(22, 22));
  grid_button->setToolTip(QObject::tr("Grid View"));

  auto* view_group = new QButtonGroup(dlg);  // Exclusive by default.
  view_group->addButton(list_button, 0);     // id 0 = List View
  view_group->addButton(grid_button, 1);     // id 1 = Grid View

  // Restore the last-used view (0 = List, 1 = Grid), defaulting to Grid the first time. The final
  // populate() below reads the checked button.
  const int saved_view = Settings::GetQSettings()
                             .value(QStringLiteral("texturehashbrowser/viewmode"), 1)
                             .toInt();
  const int initial_view = (saved_view == 0) ? 0 : 1;
  (initial_view == 0 ? list_button : grid_button)->setChecked(true);
  view_stack->setCurrentIndex(initial_view);

  auto selected_hashes = std::make_shared<std::unordered_set<u64>>(
      config.initial_selected_hashes.begin(), config.initial_selected_hashes.end());
  auto scanned_hashes = std::make_shared<std::unordered_map<u64, std::string>>();

  const auto notify_live_selection = [selected_hashes, config]() {
    if (!config.live_selection_changed)
      return;

    std::vector<u64> hashes(selected_hashes->begin(), selected_hashes->end());
    std::sort(hashes.begin(), hashes.end());
    config.live_selection_changed(hashes);
  };

  const auto populate = [tree, grid, view_group, info, selected_hashes, scanned_hashes,
                         continuous_scan_check, config, notify_live_selection]() {
    notify_live_selection();
    const QString current_label =
        config.fetch_current_label ? config.fetch_current_label() : config.current_label;

    const auto textures = config.fetch_current_entries ? config.fetch_current_entries() :
                                                         std::vector<TextureHashBrowserEntry>{};

    std::unordered_map<u64, TextureHashBrowserEntry> captured;
    captured.reserve(textures.size());
    for (const auto& tex : textures)
    {
      captured.emplace(tex.hash, tex);
      if (continuous_scan_check->isChecked())
      {
        auto& saved_name = (*scanned_hashes)[tex.hash];
        if (saved_name.empty() && !tex.name.empty())
          saved_name = tex.name;
      }
    }

    std::vector<u64> texture_hashes;
    texture_hashes.reserve(textures.size() + selected_hashes->size() + scanned_hashes->size());
    for (const auto& [captured_hash, _] : captured)
      texture_hashes.push_back(captured_hash);
    for (u64 saved_hash : *selected_hashes)
      texture_hashes.push_back(saved_hash);
    for (const auto& [seen_hash, _] : *scanned_hashes)
      texture_hashes.push_back(seen_hash);
    std::sort(texture_hashes.begin(), texture_hashes.end());
    texture_hashes.erase(std::unique(texture_hashes.begin(), texture_hashes.end()),
                         texture_hashes.end());

    const auto preview_paths = FindTexturePreviewPaths(texture_hashes);

    const bool grid_mode = (view_group->checkedId() == 1);

    // Block signals so programmatic clears/selects don't fire itemChanged / itemSelectionChanged.
    const QSignalBlocker tree_blocker(tree);
    const QSignalBlocker grid_blocker(grid);
    tree->clear();
    grid->clear();

    int saved_only_count = 0;
    int scanned_only_count = 0;
    for (u64 hash : texture_hashes)
    {
      const auto captured_it = captured.find(hash);
      const bool in_current_scene = (captured_it != captured.end());
      const bool is_saved_only = !in_current_scene && selected_hashes->count(hash) > 0;
      if (is_saved_only)
        ++saved_only_count;
      const bool is_scanned_only = !in_current_scene && !is_saved_only &&
                                   scanned_hashes->find(hash) != scanned_hashes->end();
      if (is_scanned_only)
        ++scanned_only_count;

      // Display name is the same in both views.
      QString name_text;
      if (in_current_scene)
      {
        const auto& tex = captured_it->second;
        name_text = tex.name.empty() ? QObject::tr("(unknown)") : QString::fromStdString(tex.name);
      }
      else if (is_saved_only)
      {
        name_text = QObject::tr("(saved filter, not in current selection)");
      }
      else
      {
        const auto scanned_it = scanned_hashes->find(hash);
        const bool has_scanned_name =
            scanned_it != scanned_hashes->end() && !scanned_it->second.empty();
        name_text = has_scanned_name ?
                        QString::fromStdString(scanned_it->second) :
                        QObject::tr("(seen during scan, not in current selection)");
      }
      const bool gray = !in_current_scene;

      const QString hash_hex = ToHashHex(hash);
      const auto preview_it = preview_paths.find(hash);
      const QString preview_path =
          preview_it != preview_paths.end() ? preview_it->second : QString();

      if (grid_mode)
      {
        auto* item = new QListWidgetItem(grid);
        item->setText(QStringLiteral("%1\n%2").arg(hash_hex, name_text));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(QStringLiteral("%1\n%2").arg(hash_hex, name_text));
        item->setData(Qt::UserRole, static_cast<qulonglong>(hash));
        item->setSizeHint(QSize(GRID_CARD_W, GRID_CARD_H));
        if (gray)
          item->setForeground(QColor(140, 140, 140));
        if (!preview_path.isEmpty())
        {
          const QPixmap pixmap = LoadPreviewPixmapFresh(preview_path);
          if (!pixmap.isNull())
          {
            item->setIcon(QIcon(pixmap.scaled(GRID_THUMB_SIZE, GRID_THUMB_SIZE, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation)));
          }
        }
        // Reflect the current selection; clicking the cell later toggles it (see itemSelectionChanged).
        item->setSelected(selected_hashes->count(hash) > 0);
        continue;
      }

      auto* item = new QTreeWidgetItem;
      item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
      item->setCheckState(0, selected_hashes->count(hash) > 0 ? Qt::Checked : Qt::Unchecked);
      item->setText(1, hash_hex);
      item->setText(2, name_text);
      if (gray)
      {
        const QBrush gray_brush(QColor(140, 140, 140));
        item->setForeground(1, gray_brush);
        item->setForeground(2, gray_brush);
      }

      item->setData(0, Qt::UserRole, static_cast<qulonglong>(hash));

      if (!preview_path.isEmpty())
      {
        QPixmap pixmap = LoadPreviewPixmapFresh(preview_path);
        if (!pixmap.isNull())
        {
          item->setData(3, Qt::DecorationRole,
                        pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
          item->setSizeHint(3, QSize(72, 72));
        }
      }

      tree->addTopLevelItem(item);
    }

    if (!grid_mode)
    {
      for (int i = 0; i < 4; ++i)
        tree->resizeColumnToContents(i);
    }

    if (textures.empty())
    {
      if (saved_only_count > 0 || scanned_only_count > 0)
      {
        info->setText(QObject::tr(
            "%1\nSaved filters and previously scanned textures are shown in gray.")
                          .arg(config.empty_info_text));
      }
      else
      {
        info->setText(config.empty_info_text);
      }
    }
    else
    {
      info->setText(QObject::tr(
          "%1 current texture(s) for %4. %2 saved-only and %3 scanned-only shown in gray.\n"
          "Check rows and press Apply to update Texture Filters.")
                        .arg(textures.size())
                        .arg(saved_only_count)
                        .arg(scanned_only_count)
                        .arg(current_label));
    }
  };

  QObject::connect(tree, &QTreeWidget::itemChanged, dlg,
                   [selected_hashes, notify_live_selection](QTreeWidgetItem* item, int column) {
                     if (!item || column != 0)
                       return;

                     const u64 texture_hash = item->data(0, Qt::UserRole).toULongLong();
                     const bool enabled = item->checkState(0) == Qt::Checked;
                     if (enabled)
                       selected_hashes->insert(texture_hash);
                     else
                       selected_hashes->erase(texture_hash);

                     notify_live_selection();
                   });

  // Grid view: the highlighted (selected) cells are the chosen hashes. Every relevant hash is shown
  // as a cell, so the selection set can be rebuilt directly from it.
  QObject::connect(grid, &QListWidget::itemSelectionChanged, dlg,
                   [grid, selected_hashes, notify_live_selection]() {
                     selected_hashes->clear();
                     for (const QListWidgetItem* item : grid->selectedItems())
                       selected_hashes->insert(item->data(Qt::UserRole).toULongLong());
                     notify_live_selection();
                   });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  auto* refresh_button = buttons->addButton(QObject::tr("Refresh"), QDialogButtonBox::ActionRole);
  auto* apply_button = buttons->addButton(QObject::tr("Apply"), QDialogButtonBox::AcceptRole);

  QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
  QObject::connect(refresh_button, &QPushButton::clicked, dlg, populate);
  QObject::connect(scan_timer, &QTimer::timeout, dlg, populate);
  QObject::connect(continuous_scan_check, &QCheckBox::toggled, dlg, [scan_timer, populate](bool checked) {
    if (checked)
    {
      populate();
      scan_timer->start();
    }
    else
    {
      scan_timer->stop();
    }
  });
  QObject::connect(apply_button, &QPushButton::clicked, dlg, [selected_hashes, config]() {
    if (!config.apply_selected_hashes)
      return;

    std::vector<u64> hashes(selected_hashes->begin(), selected_hashes->end());
    std::sort(hashes.begin(), hashes.end());
    config.apply_selected_hashes(hashes);
  });

  QObject::connect(view_group, &QButtonGroup::idClicked, dlg, [view_stack, populate](int id) {
    view_stack->setCurrentIndex(id);
    Settings::GetQSettings().setValue(QStringLiteral("texturehashbrowser/viewmode"), id);
    populate();  // Build the newly-shown view.
  });

  auto* layout = new QVBoxLayout;
  layout->addWidget(info);
  auto* view_row = new QHBoxLayout;
  view_row->setSpacing(0);  // Buttons sit flush, like a segmented control.
  view_row->addStretch();
  view_row->addWidget(list_button);
  view_row->addWidget(grid_button);
  layout->addLayout(view_row);
  layout->addWidget(view_stack);
  auto* bottom_layout = new QHBoxLayout;
  bottom_layout->addWidget(continuous_scan_check);
  if (preview_mode_combo)
    bottom_layout->addWidget(preview_mode_combo);
  bottom_layout->addStretch();
  bottom_layout->addWidget(buttons);
  layout->addLayout(bottom_layout);
  dlg->setLayout(layout);

  // Sync the initial preview mode (the combo defaults to Skip).
  if (preview_mode_combo)
    config.preview_mode_changed(preview_mode_combo->currentData().toBool());

  populate();
  QTimer::singleShot(200, dlg, populate);
  dlg->show();
  return dlg;
}
