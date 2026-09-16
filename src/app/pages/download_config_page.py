# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
#
# Silent X Craft Launcher (SXCL) is a third-party Minecraft launcher developed
# by the Silent X Craft Launcher Dev team, operating under the management of
# SilentCodeTeams, and overseen by SilentStudio.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version, WITH the Additional Terms described
# in the LICENSE file accompanying this program.
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Download configuration page — PCL2-style accordion menu."""

from __future__ import annotations

import threading
from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtWidgets import (
    QHBoxLayout, QVBoxLayout, QWidget, QPushButton, QLineEdit, QFrame,
)
from qfluentwidgets import (
    BodyLabel, CardWidget, InfoBar, InfoBarPosition,
    PrimaryPushButton, ComboBox,
)

from src.app.common.base_page import BasePage
from src.app.widgets.loader_row import LoaderRow
from src.app.widgets.section_card import SectionCard as AccordionSection
from src.app.theme import token
from src.services.mod_loader.compat import check_selection
from src.services.minecraft.manifest import GameVersion
from src.services.mod_loader.api import (
    fetch_forge_versions, fetch_fabric_versions,
    filter_neoforge_by_mc_version, check_optifine,
)


# ── Loader fetch worker ──────────────────────────────────────────


class LoaderFetchWorker(QThread):
    finished = Signal(dict)
    error = Signal(str)

    def __init__(self, mc_version: str):
        super().__init__()
        self.mc_version = mc_version

    def _with_fallback(self, name: str, primary) -> list:
        """某个加载器在主源搜出空列表时，换源再搜一遍。

        「无可用版本」多数是那个源抽了/接口挂了，不该直接告诉用户「这个版本没有它」——
        所以对 Forge / Fabric / NeoForge 都补一条官方源的路子（OptiFine 只有 BMCLAPI 有接口，
        没得换，就如实返回空）。
        """
        from src.core.logger import log

        try:
            items = primary() or []
        except Exception as exc:
            log.warning("[加载器] %s 主源失败: %s，换源重试", name, exc)
            items = []
        if items:
            return items

        log.info("[加载器] %s 主源没有版本，换源重搜（%s）", name, self.mc_version)
        try:
            if name == 'fabric':
                from src.core.net import fetch_json
                data = fetch_json("https://meta.fabricmc.net/v2/versions/loader/" + self.mc_version)
                return data if isinstance(data, list) else []
            if name == 'forge':
                from src.services.mod_loader.api import ForgeAPI
                versions = ForgeAPI._xml_versions(
                    "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml")
                prefix = self.mc_version + '-'
                return [{'version': v.split('-', 1)[1]} for v in versions if v.startswith(prefix)]
            if name == 'neoforge':
                from src.services.mod_loader.api import fetch_neoforge_versions
                return [item for item in fetch_neoforge_versions()
                        if str(item.get('version', '')).startswith(self.mc_version + '.')]
        except Exception as exc:
            log.warning("[加载器] %s 换源重搜也失败: %s", name, exc)
        return []

    def run(self):
        """抓四个加载器的版本列表。

        optifine 必须给**列表**：以前给的是 bool，页面拿去迭代直接炸
        （TypeError: bool object is not iterable）。
        """
        try:
            from src.services.mod_loader.api import fetch_optifine_versions

            result = {
                'forge': self._with_fallback('forge', lambda: fetch_forge_versions(self.mc_version)),
                'fabric': self._with_fallback('fabric', lambda: fetch_fabric_versions(self.mc_version, only_stable=False)),
                'neoforge': self._with_fallback('neoforge', lambda: filter_neoforge_by_mc_version(self.mc_version)),
                'optifine': fetch_optifine_versions(self.mc_version),
            }
            self.finished.emit(result)
        except Exception as e:
            self.error.emit(str(e))


# ── Accordion Section ────────────────────────────────────────────


class DownloadConfigPage(BasePage):
    """版本配置页 — PCL2 风格手风琴菜单."""

    def __init__(self, version: GameVersion, parent=None):
        super().__init__(title=f"安装 {version.id}", subtitle="", parent=parent)
        self.version = version
        self._forge_versions = []
        self._fabric_versions = []
        self._neoforge_versions = []
        self._worker = None
        self._loader_failed = set()      # 哪些加载器的列表没取到
        self._compat_issues = []         # 兼容性判定结果
        # 四个加载器的版本列表：兼容性判定会用，加载完成前先给空列表
        self._forge_versions: list = []
        self._fabric_versions: list = []
        self._neoforge_versions: list = []
        self._optifine_versions: list = []
        self._name_taken = False
        self._user_edited_name = False
        self._selected_loader = None
        self._selected_loader_version = None

        self._build_content()
        self._style_name_input("normal")
        from src.app.theme import on_theme_changed
        on_theme_changed(self._apply_theme_styles)
        self._load_loader_versions_async()

    def _apply_theme_styles(self) -> None:
        """主题切换时把本页那些"要按状态重算"的样式再套一遍。"""
        self._style_name_input("error" if self.warning_label.isVisible() else "normal")

    def _build_content(self):
        # ── Back button ──
        back = QPushButton("←  返回版本列表", self.view)
        back.setCursor(Qt.PointingHandCursor)
        from src.app.theme import token
        back.setStyleSheet(f"""
            QPushButton {{ border: none; color: {token('accent')}; font-size: 13px;
                          padding: 8px 0; text-align: left; }}
            QPushButton:hover {{ color: {token('accent')}; }}
        """)
        back.clicked.connect(self._go_back)
        self.vBoxLayout.insertWidget(0, back)

        # ── Section 1: 版本名称 ──
        self._name_section = AccordionSection("版本名称", "📝", self.view)
        self.name_input = QLineEdit(self.view)
        self.name_input.setPlaceholderText("输入自定义版本名称…")
        self.name_input.setText(self.version.id)      # 选了加载器后会按 PCL 规则重算默认名
        self.name_input.textChanged.connect(self._on_name_manual_edit)

        self.warning_label = BodyLabel("⚠ 不能与现有版本名相同", self.view)
        from src.app.theme import token as _tk
        self.warning_label.setTextColor(_tk("danger"))
        self.warning_label.setVisible(False)
        self._name_section.add_content(self.name_input)
        self._name_section.add_content(self.warning_label)
        self._name_section.set_expanded(True)
        self.add_content(self._name_section)

        # ── Section 2: 模组加载器 ──
        self._loader_section = AccordionSection("模组加载器", "🔧", self.view)

        # Forge
        self.forge_row = LoaderRow("forge", "Forge", self.view)
        self.forge_row.loader_selected.connect(self._on_loader_selected)
        self.forge_row.loader_cleared.connect(self._on_loader_cleared)
        self._loader_section.add_content(self.forge_row)


        # NeoForge
        self.neoforge_row = LoaderRow("neoforge", "NeoForge", self.view)
        self.neoforge_row.loader_selected.connect(self._on_loader_selected)
        self.neoforge_row.loader_cleared.connect(self._on_loader_cleared)
        self._loader_section.add_content(self.neoforge_row)


        # Fabric
        self.fabric_row = LoaderRow("fabric", "Fabric", self.view)
        self.fabric_row.loader_selected.connect(self._on_loader_selected)
        self.fabric_row.loader_cleared.connect(self._on_loader_cleared)
        self._loader_section.add_content(self.fabric_row)
        self.optifine_row = LoaderRow("optifine", "OptiFine", self.view)
        self.optifine_row.loader_selected.connect(self._on_loader_selected)
        self.optifine_row.loader_cleared.connect(self._on_loader_cleared)
        self._loader_section.add_content(self.optifine_row)


        # OptiFine
        of_layout = QHBoxLayout()
        of_layout.setContentsMargins(8, 4, 8, 4)
        of_label = BodyLabel("OptiFine", self.view)
        of_label.setFixedWidth(64)
        of_layout.addWidget(of_label)
        self.optifine_status = BodyLabel("检测中…", self.view)
        of_layout.addWidget(self.optifine_status)
        of_layout.addStretch()
        self._loader_section.add_layout(of_layout)

        self._loader_section.set_expanded(False)
        self._loader_rows = [self.forge_row, self.neoforge_row, self.fabric_row, self.optifine_row]
        for row in self._loader_rows:
            row.set_group(self._loader_rows)
            row.expanded.connect(self._on_loader_expanded)

        self.add_content(self._loader_section)

        # ── Section 3: 下载按钮 ──
        download_card = CardWidget(self.view)
        dl_layout = QHBoxLayout(download_card)
        dl_layout.setContentsMargins(0, 0, 0, 0)
        # 兼容性提示区：不兼容时这里写清原因，按钮同时禁用（不允许进入下一步）
        self.compat_box = QWidget(download_card)
        self._compat_layout = QVBoxLayout(self.compat_box)
        self._compat_layout.setContentsMargins(0, 0, 0, 6)
        self._compat_layout.setSpacing(4)
        dl_layout.addWidget(self.compat_box)

        self.download_btn = PrimaryPushButton("开始下载", download_card)
        self.download_btn.setFixedHeight(44)
        self.download_btn.clicked.connect(self._on_download)
        dl_layout.addWidget(self.download_btn)
        self.add_content(download_card)

        self.add_stretch()

    def _on_loader_expanded(self, row) -> None:
        """展开一行就把别的收起来（手风琴），避免好几个列表同时撑开页面。"""
        for other in self._loader_rows:
            if other is not row and other._expanded:
                other.set_expanded(False)

    # ── Loader data ─────────────────────────────────────────────

    def _load_loader_versions_async(self):
        self._worker = LoaderFetchWorker(self.version.id)
        self._worker.finished.connect(self._on_loader_loaded)
        self._worker.error.connect(self._on_loader_error)
        self._worker.start()

    def _on_loader_loaded(self, result):
        self._forge_versions = result.get('forge', [])
        self._fabric_versions = result.get('fabric', [])
        self._neoforge_versions = result.get('neoforge', [])
        self._optifine_versions = result.get('optifine', []) or []
        has_opti = bool(self._optifine_versions)

        self.forge_row.set_versions(self._forge_versions, 'version')
        self.fabric_row.set_versions(self._fabric_versions, 'loader.version')
        self.neoforge_row.set_versions(self._neoforge_versions, 'version')
        # OptiFine 的版本号要拼成 PCL 那种 HD_U_J8_pre12（type_patch），
        # 只给 pre12 的话版本名会变成 1.21.11-OptiFine_pre12，和 PCL 对不上
        opti_normalized = []
        for item in self._optifine_versions:
            if not isinstance(item, dict):
                continue
            type_name = str(item.get('type') or '').strip()
            patch = str(item.get('patch') or '').strip()
            label = f'{type_name}_{patch}' if type_name and patch else (patch or type_name)
            if not label:
                continue
            entry = dict(item)
            entry['version'] = label
            opti_normalized.append(entry)
        self.optifine_row.set_versions(opti_normalized, 'version')
        self._loader_failed = set()
        self._refresh_compat()

        self.optifine_status.setText("✅ 支持" if has_opti else "—")
        from src.app.theme import token
        self.optifine_status.setStyleSheet(
            f"color: {token('success')};" if has_opti else f"color: {token('text_tertiary')};"
        )

    def _on_loader_error(self, error):
        for row in self._loader_rows:
            row.set_error("加载失败")
        self._loader_failed = {'forge', 'fabric', 'neoforge', 'optifine'}
        self._refresh_compat()

    # ── Loader selection ────────────────────────────────────────

    def _on_loader_selected(self, ltype: str, version: str):
        for row in self._loader_rows:
            if row.loader_type != ltype and row.is_selected():
                row._clear()
        self._selected_loader = ltype
        self._selected_loader_version = version
        self._update_version_name()

    def _on_loader_cleared(self, ltype: str):
        self._compat_issues = []
        self._selected_loader = None
        self._selected_loader_version = None
        self._update_version_name()

    def _update_version_name(self):
        """默认版本名按 PCL 的规则拼（GetSelectName）。

        以前是 f"{原版}-{加载器}-{版本号}"，和 PCL 装出来的名字不一样 ——
        从 PCL 迁过来的用户会看到两套命名，同一个版本像是装了两次。
        现在逐字对齐它：Fabric 后面是空格、Forge/NeoForge/OptiFine 用下划线、
        LiteLoader 不带版本号、顺序固定 Fabric -> Forge -> NeoForge -> LiteLoader -> OptiFine。
        """
        base = self.version.id
        try:
            from src.services.minecraft.loaders import LoaderKind, default_version_name
            loaders = {}
            if self._selected_loader and self._selected_loader_version:
                key = {LoaderKind.FORGE: LoaderKind.FORGE, LoaderKind.NEOFORGE: LoaderKind.NEOFORGE,
                       LoaderKind.FABRIC: LoaderKind.FABRIC, LoaderKind.OPTIFINE: LoaderKind.OPTIFINE,
                       LoaderKind.LITELOADER: LoaderKind.LITELOADER,
                       "forge": LoaderKind.FORGE, "neoforge": LoaderKind.NEOFORGE,
                       "fabric": LoaderKind.FABRIC, "optifine": LoaderKind.OPTIFINE,
                       "liteloader": LoaderKind.LITELOADER}.get(self._selected_loader)
                if key:
                    loaders[key] = str(self._selected_loader_version)
            vn = default_version_name(base, loaders) if loaders else base
        except Exception:
            vn = (f"{base}-{self._selected_loader}-{self._selected_loader_version}"
                  if self._selected_loader and self._selected_loader_version else base)
        if not getattr(self, '_user_edited_name', False):
            self.name_input.blockSignals(True)
            self.name_input.setText(vn)
            self.name_input.blockSignals(False)
        self.name_input.setPlaceholderText(f"自动生成: {vn}")
        self._check_version_exists(vn)
        self._refresh_compat()

    def _style_name_input(self, state: str = "normal") -> None:
        """版本名输入框的三种状态（普通 / 获得焦点 / 重名错误），全部走主题令牌。"""
        from src.app.theme import token
        border = {"normal": token("input_border"), "error": token("danger")}.get(state, token("input_border"))
        width = "2px" if state == "error" else "1px"
        self.name_input.setStyleSheet(
            f"QLineEdit {{ border: {width} solid {border}; border-radius: 6px; padding: 8px 12px;"
            f" font-size: 13px; background: {token('input_bg')}; color: {token('text')}; }}"
            f"QLineEdit:focus {{ border-color: {token('accent')}; }}")

    def _refresh_compat(self) -> None:
        """重新算一遍兼容性，并把结论画到提示区（有 error 就不能点下载）。"""
        try:
            from src.services.minecraft.loaders import installed_for_base
            installed = installed_for_base(self.version.id)
        except Exception:
            installed = []

        meta = None
        if self._selected_loader == 'optifine' and self._selected_loader_version:
            for item in getattr(self, '_optifine_versions', []):
                if isinstance(item, dict) and item.get('version') == self._selected_loader_version:
                    meta = item
                    break

        self._compat_issues = check_selection(
            base_version=self.version.id,
            loader_type=self._selected_loader,
            loader_version=self._selected_loader_version,
            loader_versions={
                'forge': getattr(self, '_forge_versions', []),
                'fabric': getattr(self, '_fabric_versions', []),
                'neoforge': getattr(self, '_neoforge_versions', []),
                'optifine': getattr(self, '_optifine_versions', []),
            },
            loader_failed=self._loader_failed,
            optifine_meta=meta,
            installed_for_base=installed,
        )

        while self._compat_layout.count():
            item = self._compat_layout.takeAt(0)
            widget = item.widget() if item is not None else None
            if widget is not None:
                widget.deleteLater()

        for issue in self._compat_issues:
            label = BodyLabel(('✗ ' if issue.is_error else '⚠ ') + issue.message
                              + (('（' + issue.fix + '）') if issue.fix else ''), self.compat_box)
            label.setWordWrap(True)
            color = token('danger') if issue.is_error else token('warning')
            label.setStyleSheet(f'color: {color};')
            self._compat_layout.addWidget(label)
        self.compat_box.setVisible(bool(self._compat_issues))
        self._refresh_download_button()

    def _refresh_download_button(self) -> None:
        """按钮状态只在一处决定：重名 / 不兼容 / 正常。"""
        if any(issue.is_error for issue in self._compat_issues):
            self.download_btn.setEnabled(False)
            self.download_btn.setText('⚠ 不兼容，无法安装')
            return
        if getattr(self, '_name_taken', False):
            self.download_btn.setEnabled(False)
            self.download_btn.setText('⚠ 版本已存在')
            return
        self.download_btn.setEnabled(True)
        self.download_btn.setText('开始下载')

    def _check_version_exists(self, version_name: str):
        """目录里有 JSON 就算已存在 —— Forge 1.13+/Fabric 装的版本没有自己的 jar。"""
        from pathlib import Path
        from src.app.common.launcher_config import cfg
        d = Path(cfg.gameDirectory.value) / "versions" / version_name
        has_json = d.is_dir() and any(p.suffix == ".json" for p in d.glob("*.json"))
        self._name_taken = bool(has_json or (d / f"{version_name}.jar").exists())
        if self._name_taken:
            self._style_name_input("error")
            self.warning_label.setVisible(True)
        else:
            self._style_name_input("normal")
            self.warning_label.setVisible(False)
        self._refresh_download_button()

    def _on_name_manual_edit(self, text: str):
        ph = self.name_input.placeholderText()
        self._user_edited_name = bool(text and text != ph and text != self.version.id)
        # 每次用户打字都检测重名
        self._check_version_exists(text.strip())

    def _go_back(self):
        mw = self.window()
        if hasattr(mw, 'go_back_to_versions'):
            mw.go_back_to_versions()
        else:
            self.parent().switchTo(self.parent().versions_page)

    def _confirm_same_loader(self, loader_type: str, loader_version) -> bool:
        """已装了同类加载器时问一句：直接启动旧的，还是再装一个？

        返回 True 表示「用户选择启动已装的，这次不要装了」。
        用户经常在同一个原版下装好几套（Forge + OptiFine、Fabric + OptiFine…），
        所以这里不阻止，只把"已经装了什么"摆出来，并给出直接启动的快捷方式。
        """
        try:
            from src.services.minecraft.loaders import installed_for_base
            existing = installed_for_base(self.version.id)
        except Exception as exc:
            log.debug("同版本加载器检测跳过: %s", exc)
            return False

        same = [item for item in existing if item.has_loader(loader_type)]
        if not same:
            return False

        from PySide6.QtWidgets import QMessageBox
        summary = "、".join(item.summary() for item in same)
        box = QMessageBox(self)
        box.setWindowTitle("这个版本已经装了同类加载器")
        box.setIcon(QMessageBox.Information)
        box.setText(f"{self.version.id} 下已经有：{summary}")
        box.setInformativeText(
            "两个选择：\n"
            "· 直接启动已经装好的那一个（不再下载任何东西）\n"
            "· 继续再装一个：会在 versions/ 下新建独立目录，两套互不影响")
        launch_btn = box.addButton("直接启动已装的", QMessageBox.AcceptRole)
        box.addButton("继续再装一个", QMessageBox.RejectRole)
        box.exec()
        if box.clickedButton() is launch_btn:
            self._launch_installed(same[0])
            return True
        return False

    def _launch_installed(self, installed) -> None:
        """直接启动已经装好的那一份（不再重新下载）。"""
        try:
            mw = self.window()
            if hasattr(mw, "switch_to_launch"):
                mw.switch_to_launch(self.version)
                InfoBar.success(title="启动已装好的版本", content=installed.summary(),
                                orient=InfoBarPosition.TOP, isClosable=True, duration=3000,
                                parent=self)
                return
        except Exception as exc:
            log.warning("启动已装版本失败: %s", exc)
        InfoBar.info(title="已经装好了", content=f"去版本列表启动 {installed.id}",
                     orient=InfoBarPosition.TOP, isClosable=True, duration=4000, parent=self)

    def _on_download(self):
        vn = self.name_input.text().strip()
        if not vn:
            InfoBar.warning(title="请输入版本名称", content="版本名称不能为空",
                            orient=InfoBarPosition.TOP, isClosable=True, duration=3000, parent=self)
            return

        from pathlib import Path
        from src.app.common.launcher_config import cfg
        d = Path(cfg.gameDirectory.value) / "versions" / vn
        has_json = d.is_dir() and any(p.suffix == ".json" for p in d.glob("*.json"))
        if has_json or (d / f"{vn}.jar").exists():
            InfoBar.warning(title="版本已存在", content=f"版本 '{vn}' 已经安装，请使用不同的版本名称",
                            orient=InfoBarPosition.TOP, isClosable=True, duration=5000, parent=self)
            self._style_name_input("error")
            return

        lt, lv = "none", None
        if self._selected_loader == 'forge':
            lv = self.forge_row.get_selected_version()
            if lv: lt = 'forge'
        elif self._selected_loader == 'neoforge':
            lv = self.neoforge_row.get_selected_version()
            if lv: lt = 'neoforge'
        elif self._selected_loader == 'fabric':
            lv = self.fabric_row.get_selected_version()
            if lv: lt = 'fabric'
        elif self._selected_loader == 'optifine':
            lv = self.optifine_row.get_selected_version()
            if lv: lt = 'optifine'

        # 装之前先看一眼：同一个原版下是不是已经有同类加载器了
        if lt != "none" and self._confirm_same_loader(lt, lv):
            return

        # 最后一道闸：有 error 一律不许进下一步（界面禁用之外再兜一次）
        errors = [issue for issue in self._compat_issues if issue.is_error]
        if errors:
            InfoBar.error(title='无法开始安装', content=errors[0].message,
                          orient=InfoBarPosition.TOP, isClosable=True, duration=6000, parent=self)
            return

        mw = self.window()
        if hasattr(mw, 'switch_to_download_progress'):
            mw.switch_to_download_progress(version=self.version, version_name=vn,
                                           loader_type=lt, loader_version=lv)
        else:
            # 旧版"就地安装"回退路径已删除：它依赖已被移除的 VersionInstaller。
            InfoBar.error(title="无法开始下载", content="主窗口未提供下载进度页接口",
                          orient=InfoBarPosition.TOP, isClosable=True,
                          duration=5000, parent=self)
