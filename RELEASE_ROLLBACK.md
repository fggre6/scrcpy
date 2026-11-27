# 发布记录与回档对照表

仓库：`fggre6/scrcpy`　记录时间：2026-11-27

## 清理计划与当前状态

- 保留：**最新 1 个 Release** —— `build-20261127-102425`
- 待删除：其余 **29 个 Release**（含全部 6 个 `server-*` 发布）
- **全部 31 个 git tag 原样保留**，包括没有对应 Release 的 `build-20261011-204850`
- 当前删除状态：**尚未执行**。当前环境的 GitHub 工具不支持删除 Release，直连 GitHub API 也被环境策略拦截；下表已完整记录节点，删除可由用户在 GitHub 网页或本地已认证的 `gh` CLI 中按表执行。

## 回档方法

```bash
# 方式一：用 tag 回档（tag 全部保留，推荐）
git checkout <tag_name>

# 方式二：用表中 commit SHA 回档（即使将来 tag 被删除也可用）
git checkout <commit_sha>

# 从某个历史节点重新打包（示例）
git checkout <tag_name> && ./release/package_client.sh
```

## 发布 → 节点对照

按发布时间倒序。`提交说明`为对应 commit 的标题；大量构建多次重复指向同一节点 `4cd633fb`，这是正常现象。

| 发布时间 (UTC) | Tag / Release | Commit | 提交说明 | 处理 |
|---|---|---|---|---|
| 2026-11-27 10:24:30 | `build-20261127-102425` | `5623beb9b0aac3f94fa143f0e721b203d03d9e39` | fix(motion): flush pending delta on tick to cut view-mode latency | **保留** |
| 2026-11-27 08:24:28 | `build-20261127-082422` | `47e93c003f403d6f7fd62771eed3cba582b2788f` | ci: trigger build on master pushes | 待删除 |
| 2026-11-22 07:56:54 | `build-20261122-075647` | `60d8274865057b18e52d71804acb6d7cbf1d7f99` | fix(mouse): fall back to upstream clicks when mapping dependency is unmet | 待删除 |
| 2026-11-21 07:34:15 | `build-20261121-073409` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-20 19:09:06 | `build-20261120-190858` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-20 19:04:28 | `build-20261120-190422` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-20 18:40:37 | `build-20261120-184032` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-20 17:03:03 | `build-20261120-170256` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-20 16:40:52 | `build-20261120-164044` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-20 16:07:34 | `build-20261120-160727` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-20 15:35:58 | `build-20261120-153551` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-07 08:55:37 | `build-20261107-085532` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-07 08:49:03 | `build-20261107-084856` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-07 08:47:23 | `build-20261107-084716` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-07 08:46:07 | `build-20261107-084601` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-07 08:39:51 | `build-20261107-083945` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-07 08:16:49 | `build-20261107-081644` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-07 08:04:31 | `build-20261107-080426` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-11-06 17:50:40 | `build-20261106-175032` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-10-13 13:12:05 | `build-20261013-131157` | `baaf6362b0906fe22172a885113a410b44794c28` | Rename build-Windows to build-Windows.yml | 待删除 |
| 2026-10-12 13:36:12 | `build-20261012-133605` | `32816752682f020344b1be524852aacf4e0f0ed4` | Update input_manager.c | 待删除 |
| 2026-10-12 09:15:21 | `server-20261012-091516` | `4cd633fbf2cca53766ed13ffa2c069dea6a3243e` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-10-12 08:55:35 | `server-20261012-085531` | `8a07796c73785ede3646b3d64bd74fc4151fbad2` | Update Controller.java | 待删除 |
| 2026-10-12 08:36:40 | `server-20261012-083635` | `cac5fcd23fb759470c910edbcf4415bf157f8eb9` | Update Controller.java | 待删除 |
| 2026-10-12 07:57:04 | `server-20261012-075701` | `a124b73e0c6c1f93bc739f6247c509f004002b1c` | Update Controller.java | 待删除 |
| 2026-10-12 07:34:30 | `server-20261012-073426` | `ff2986e7c13e2f53ba2da52957e0206abb3f9807` | Refactor touch event injection logic in Controller | 待删除 |
| 2026-10-12 07:18:18 | `server-20261012-071815` | `4f18ad52c4b6277cfc15856a5d09779f600f1dbc` | Correct log message formatting in injectTouch | 待删除 |
| 2026-10-12 05:49:13 | `build-20261012-054906` | `a1674c1b5725e3cd8bbbbb05922b3575dbb5ffc3` | Refactor build.yml for server compilation steps | 待删除 |
| 2026-10-12 04:34:04 | `build-20261012-043358` | `ca664f887af3bf4e37a0c27c871582ca9994f4f2` | Update input_manager.c | 待删除 |
| 2026-10-12 03:58:02 | `build-20261012-035757` | `b0a69eeeaee02e2cbf1c853767b9d97064b1fd1a` | Update input_manager.c | 待删除 |

## 只有 tag、没有 Release 的节点

| Tag | Commit | 提交说明 |
|---|---|---|
| `build-20261011-204850` | `a917174fa0f0470392c8b2e47f1ec047c66b7385` | Update custom key mappings and touch event handling |

## 如需手动删除 Release

在已登录 GitHub 且已认证 `gh` 的机器上执行（git tag 不受影响）：

```bash
# 删除全部 29 个旧发布（保留 build-20261127-102425）
for t in $(git tag | grep -v '^build-20261127-102425$'); do
  gh release delete "$t" --yes
done
```

或在 GitHub 网页：Releases → 选择旧发布 → Delete release。
