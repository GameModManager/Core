#include "ui/widgets/mod_list_sort_proxy.h"

#include "ui/widgets/mod_list_model.h"

#include <QMimeData>
#include <QVector>
#include <algorithm>

namespace ui {

namespace {
// Whether a row is a "fixed" unit that must not move: separator, game-native,
// Overwrite/MERGED (pinned). These act as band barriers.
bool is_fixed_row(const ModEntry &m) {
  return m.is_separator || m.is_game_native || m.is_overwrite || m.is_merged;
}

// Compare two ModEntry by the requested column. Returns negative/0/positive.
int compare_mods(const ModEntry &a, const ModEntry &b, int column, int srcA, int srcB) {
  auto empty_last_asc = [](const QString &x, const QString &y) -> int {
    const bool xe = x.isEmpty();
    const bool ye = y.isEmpty();
    if (xe && ye)
      return 0;
    if (xe)
      return 1;
    if (ye)
      return -1;
    return x.compare(y, Qt::CaseSensitive);
  };
  auto zero_last_asc = [](qint64 x, qint64 y) -> int {
    const bool xe = (x == 0);
    const bool ye = (y == 0);
    if (xe && ye)
      return 0;
    if (xe)
      return 1;
    if (ye)
      return -1;
    if (x < y)
      return -1;
    if (x > y)
      return 1;
    return 0;
  };

  switch (column) {
  case ModList::Fold:
    // Not sortable; identity.
    return (srcA < srcB) ? -1 : (srcA > srcB ? 1 : 0);
  case ModList::Name: {
    int c = a.name.compare(b.name, Qt::CaseInsensitive);
    if (c != 0)
      return c;
    break;
  }
  case ModList::Conflicts: {
    auto count = [](const ModEntry &m) {
      int c = 0;
      if (m.conflict_wins > 0)
        c++;
      if (m.conflict_losses > 0)
        c++;
      if (m.redundant)
        c++;
      return c;
    };
    auto mask = [](const ModEntry &m) {
      int msk = 0;
      if (m.conflict_wins > 0)
        msk |= 1;
      if (m.conflict_losses > 0)
        msk |= 2;
      if (m.redundant)
        msk |= 4;
      return msk;
    };
    int ca = count(a), cb = count(b);
    if (ca != cb)
      return (ca < cb) ? -1 : 1;
    int ma = mask(a), mb = mask(b);
    if (ma != mb)
      return (ma < mb) ? -1 : 1;
    break;
  }
  case ModList::Flags: {
    auto flagmask = [](const ModEntry &m) {
      // bit order per analysis: invalid_data, no_metadata, is_empty,
      // root_override, is_fomod, has_hidden_files, is_mirrored,
      // mirror_source_missing, update_available, toggle_error, tags
      int msk = 0;
      int bit = 0;
      if (m.invalid_data)
        msk |= (1 << bit);
      bit++;
      if (m.no_metadata)
        msk |= (1 << bit);
      bit++;
      if (m.is_empty)
        msk |= (1 << bit);
      bit++;
      if (m.root_override)
        msk |= (1 << bit);
      bit++;
      if (m.is_fomod)
        msk |= (1 << bit);
      bit++;
      if (m.has_hidden_files)
        msk |= (1 << bit);
      bit++;
      if (m.is_mirrored)
        msk |= (1 << bit);
      bit++;
      if (m.mirror_source_missing)
        msk |= (1 << bit);
      bit++;
      if (m.update_available)
        msk |= (1 << bit);
      bit++;
      if (!m.toggle_error.isEmpty())
        msk |= (1 << bit);
      bit++;
      if (!m.tags.isEmpty())
        msk |= (1 << bit);
      bit++;
      int count = 0;
      int mm = msk;
      while (mm) {
        count += mm & 1;
        mm >>= 1;
      }
      return std::make_pair(count, msk);
    };
    auto fa = flagmask(a), fb = flagmask(b);
    if (fa.first != fb.first)
      return (fa.first < fb.first) ? -1 : 1;
    if (fa.second != fb.second)
      return (fa.second < fb.second) ? -1 : 1;
    break;
  }
  case ModList::Category: {
    int c = a.category.compare(b.category, Qt::CaseInsensitive);
    // empty last ascending
    if (a.category.isEmpty() && !b.category.isEmpty())
      return 1;
    if (!a.category.isEmpty() && b.category.isEmpty())
      return -1;
    if (c != 0)
      return c;
    break;
  }
  case ModList::Source: {
    int c = a.source_type.compare(b.source_type, Qt::CaseInsensitive);
    if (c != 0)
      return c;
    // git presence: git mods after non-git? Analysis: "source type label, then
    // git presence, then name". Treat git presence as bool; non-git first
    // ascending? Use: a.is_git < b.is_git.
    if (a.is_git != b.is_git)
      return a.is_git ? 1 : -1;
    int n = a.name.compare(b.name, Qt::CaseInsensitive);
    if (n != 0)
      return n;
    break;
  }
  case ModList::SourceId: {
    bool aNum = false, bNum = false;
    qlonglong an = a.source_id.toLongLong(&aNum);
    qlonglong bn = b.source_id.toLongLong(&bNum);
    if (aNum && bNum) {
      if (an != bn)
        return (an < bn) ? -1 : 1;
    } else {
      int c = a.source_id.compare(b.source_id, Qt::CaseInsensitive);
      if (c != 0)
        return c;
    }
    int n = a.name.compare(b.name, Qt::CaseInsensitive);
    if (n != 0)
      return n;
    break;
  }
  case ModList::Version: {
    // lexical case-sensitive; empty last ascending
    if (a.version.isEmpty() && !b.version.isEmpty())
      return 1;
    if (!a.version.isEmpty() && b.version.isEmpty())
      return -1;
    int c = a.version.compare(b.version, Qt::CaseSensitive);
    if (c != 0)
      return c;
    break;
  }
  case ModList::Installation: {
    int r = zero_last_asc(a.installation_ts, b.installation_ts);
    if (r != 0)
      return r;
    break;
  }
  case ModList::Changed: {
    int r = zero_last_asc(a.changed_ts, b.changed_ts);
    if (r != 0)
      return r;
    break;
  }
  case ModList::Priority: {
    if (a.priority != b.priority)
      return (a.priority < b.priority) ? -1 : 1;
    break;
  }
  default:
    break;
  }
  // tie-break by source row
  if (srcA != srcB)
    return (srcA < srcB) ? -1 : 1;
  return 0;
}
}  // namespace

ModListSortProxy::ModListSortProxy(ModList *source, QObject *parent)
    : QSortFilterProxyModel(parent), mod_list_(source) {
  setSourceModel(source);
  setDynamicSortFilter(true);
  // default sort column priority ascending
  sort_column_     = ModList::Priority;
  display_order_   = Qt::AscendingOrder;
  rebuild_ranks();
}

void ModListSortProxy::sort(int column, Qt::SortOrder order) {
  sort_column_   = column;
  display_order_ = order;
  rebuild_ranks();
  // base with Ascending: ranks already encode direction
  QSortFilterProxyModel::sort(column, Qt::AscendingOrder);
}

void ModListSortProxy::rebuild_ranks() {
  rebuild_rank_for(sort_column_, display_order_);
}

void ModListSortProxy::rebuild_rank_for(int column, Qt::SortOrder order) {
  if (!mod_list_) {
    rank_.clear();
    return;
  }
  const auto &mods = mod_list_->mods();
  const int n = mods.size();
  rank_.assign(n, 0);

  // Build units: each unit is a block of source rows moving together.
  // A unit starts at a row whose parent is absent/empty or which is fixed.
  // Children (rows with a present parent) are folded into their parent's unit.
  QVector<QVector<int>> units;
  QVector<char> is_fixed_unit;
  QVector<int> unit_of(n, -1);
  for (int i = 0; i < n; ++i) {
    // skip children; they get added to parent unit
    const QString pid = mods[i].parent_id;
    bool is_child = false;
    if (!pid.isEmpty() && mod_list_->nesting_enabled()) {
      for (int j = 0; j < n; ++j) {
        if (j != i && mods[j].id == pid) {
          is_child = true;
          break;
        }
      }
    }
    if (is_child)
      continue;
    // start new unit
    QVector<int> block;
    block.append(i);
    // gather descendants in source order
    if (mod_list_->nesting_enabled()) {
      for (int j = i + 1; j < n; ++j) {
        if (mods[j].parent_id.isEmpty())
          continue;
        // descendant of i?
        // check chain
        QString cur = mods[j].parent_id;
        bool desc = false;
        QSet<QString> seen;
        while (!cur.isEmpty() && !seen.contains(cur)) {
          seen.insert(cur);
          if (cur == mods[i].id) {
            desc = true;
            break;
          }
          // find parent row
          QString next;
          for (int k = 0; k < n; ++k)
            if (mods[k].id == cur) {
              next = mods[k].parent_id;
              break;
            }
          cur = next;
        }
        if (desc && !block.contains(j))
          block.append(j);
      }
      std::sort(block.begin(), block.end());
    }
    // fixed?
    bool fixed = false;
    for (int r : block)
      if (is_fixed_row(mods[r]))
        fixed = true;
    units.append(block);
    is_fixed_unit.append(fixed ? 1 : 0);
    for (int r : block)
      unit_of[r] = units.size() - 1;
  }
  // Rows that were skipped as children but whose parent wasn't found (orphan)
  // - add as own unit.
  for (int i = 0; i < n; ++i) {
    if (unit_of[i] >= 0)
      continue;
    QVector<int> tmp; tmp.append(i); units.append(tmp);
    is_fixed_unit.append(is_fixed_row(mods[i]) ? 1 : 0);
    unit_of[i] = units.size() - 1;
  }

  // Now sort movable units within bands. Bands delimited by fixed units.
  // Collect indices of units.
  QVector<int> unit_order;
  unit_order.reserve(units.size());
  // We'll process: accumulate movable units; on fixed unit, sort accumulated
  // and append, then append fixed.
  QVector<int> band;
  auto flush_band = [&]() {
    // sort band movable units by comparator with direction
    std::stable_sort(band.begin(), band.end(), [&](int ua, int ub) {
      int cmp = compare_mods(mods[units[ua][0]], mods[units[ub][0]], column,
                             units[ua][0], units[ub][0]);
      if (order == Qt::DescendingOrder)
        cmp = -cmp;
      if (cmp != 0)
        return cmp < 0;
      return units[ua][0] < units[ub][0];
    });
    for (int u : band)
      unit_order.append(u);
    band.clear();
  };
  for (int ui = 0; ui < units.size(); ++ui) {
    if (is_fixed_unit[ui]) {
      flush_band();
      unit_order.append(ui);
    } else {
      band.append(ui);
    }
  }
  flush_band();

  // Flatten
  QVector<int> flat;
  flat.reserve(n);
  for (int u : unit_order)
    for (int r : units[u])
      flat.append(r);

  // rank
  for (int pos = 0; pos < flat.size(); ++pos)
    rank_[flat[pos]] = pos;
}

bool ModListSortProxy::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const {
  Q_UNUSED(source_parent);
  if (!mod_list_)
    return true;
  // fold hidden
  if (mod_list_->is_row_fold_hidden(source_row))
    return false;
  if (mod_list_->has_view_visible()) {
    return mod_list_->is_row_view_visible(source_row);
  }
  // fallback: filter_visible_ + fold
  if (!mod_list_->is_row_filter_visible(source_row))
    return false;
  return true;
}

bool ModListSortProxy::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                                       int column, const QModelIndex &parent) const {
  Q_UNUSED(action);
  Q_UNUSED(row);
  Q_UNUSED(column);
  Q_UNUSED(parent);
  if (data && data->hasFormat(QString::fromLatin1(kModListMimeType))) {
    if (!(sort_column_ == ModList::Priority && display_order_ == Qt::AscendingOrder))
      return false;
  }
  return QSortFilterProxyModel::canDropMimeData(data, action, row, column, parent);
}

bool ModListSortProxy::dropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                                    int column, const QModelIndex &parent) {
  if (data && data->hasFormat(QString::fromLatin1(kModListMimeType))) {
    if (!(sort_column_ == ModList::Priority && display_order_ == Qt::AscendingOrder)) {
      emit const_cast<ModListSortProxy *>(this)->drag_refused();
      return false;
    }
  }
  return QSortFilterProxyModel::dropMimeData(data, action, row, column, parent);
}

}  // namespace ui
