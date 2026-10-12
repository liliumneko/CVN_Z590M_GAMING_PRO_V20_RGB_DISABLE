# igame-led 安装指南
##注意：纯AI生成，没有人工痕迹

面向第三方用户的完整安装/使用说明。项目原理和协议细节见 [`docs/ANALYSIS.md`](docs/ANALYSIS.md)。

---

## 1. 这是什么

`igame-led` 是一个原生 Linux 小工具，用来**关闭（或设置）七彩虹 Colorful / iGame
主板的 RGB 灯光**。它直接对主板上的 USB HID 灯控芯片发送 65 字节的控制报文，
不需要 Windows、Wine 或 OpenRGB。

- 单一可执行文件，无运行时依赖（只用 libc）。
- 附带 udev 规则和 systemd 服务，可做到**开机自动关灯**。

---

## 2. 支持范围

| 硬件 | 支持 |
|------|------|
| Colorful / iGame 主板 RGB（HID 设备 `VID 0x2F4C`，灯控接口 usage page `0xFF01`） | ✅ |
| Colorful / iGame 显卡 RGB | ❌ Linux 下不可行 |
| 内存（ENE）、键盘、控制盒 | ❌ 未移植 |

安装程序只对 **vendor id `2F4C`** 的 HID 设备写入，不会碰其它设备。

---

## 3. 系统要求

- Linux，内核开启 `CONFIG_HIDRAW`（绝大多数发行版默认开启）。
- 编译：`gcc`、`make`、libc 开发头文件。
- 自动关灯（可选）：`udev` + `systemd`。

---

## 4. 安装（Arch Linux）

```bash
# 1) 获取源码后进入目录
cd igame-led

# 2) 编译
make

# 3) 安装（注意必须用 PREFIX=/usr）
sudo make install PREFIX=/usr
```

> ⚠️ **必须 `PREFIX=/usr`**。默认的 `PREFIX=/usr/local` 会把程序装到
> `/usr/local/bin`，而 udev 规则和 systemd 服务里写死了 `/usr/bin/igame-led`，
> 那样自动关灯不会生效。

### 权限

程序需要读写 `/dev/hidrawN`，安装的 udev 规则会把这些设备划给 `i2c` 组：

```bash
sudo groupadd -f i2c
sudo usermod -aG i2c "$USER"
sudo udevadm control --reload-rules
sudo udevadm trigger
```

然后**注销重新登录一次**（让 `i2c` 组生效），之后即可免 root 使用。

### 用 PKGBUILD（可选）

```bash
cp packaging/PKGBUILD .
makepkg -si
```

PKGBUILD 内部已用 `PREFIX=/usr` 安装，并会一并装好 udev 规则和服务。

---


## 5. 验证

```bash
# 应看到一条 usage_page=0xFF01 的设备（灯控通道）
igame-led mb list

# 关灯（会先让你确认目标设备）
igame-led mb off

# 或者先设成红色来确认通道可用，再关掉
igame-led mb set ff0000
igame-led mb off
```

若 `mb list` 有输出但 `mb off` 提示 `Permission denied`，说明 `i2c` 组还没生效——
重新登录，或临时用 `sudo igame-led mb off` 验证。

---

## 6. 开机自动关灯

安装时已经装好两个机制：

1. **udev 规则**（`/usr/lib/udev/rules.d/60-igame-led.rules`）
   在灯控设备出现的瞬间执行 `igame-led mb off --yes`。装了规则后，
   `udevadm trigger` 就会立刻生效一次。
2. **systemd 服务**（兜底）：

   ```bash
   sudo systemctl enable --now igame-led.service
   ```

只启用 udev 规则通常就够了；服务用于某些设备出现较晚的场景。两者都启用也无冲突。

---

## 7. 命令参考

```
igame-led mb list                      列出 Colorful HID 灯光设备
igame-led mb off                       关闭主板灯光
igame-led mb set <RRGGBB|r,g,b>        设置静态颜色（如 ff0000 或 255,0,0）
igame-led mb raw <hex bytes>           发送原始 HID 报文（调试用）
igame-led off                          等价于 mb off
```

选项：

| 选项 | 说明 |
|------|------|
| `--pid 0x1000` | 只对指定 PID 的 HID 设备操作 |
| `--any-usage` | 不按 usage page 过滤，向所有匹配设备发送 |
| `--dry-run` | 只打印将发送的报文，不写入 |
| `--yes` | 跳过交互确认 |

---

## 8. 卸载

```bash
sudo systemctl disable --now igame-led.service
sudo make uninstall PREFIX=/usr
sudo udevadm control --reload-rules
```

必要时再手动清理：

```bash
# 若不再需要该组
sudo groupdel i2c
# 从组里移除自己：编辑 /etc/group，或用 gpasswd -d <user> i2c
```

---

## 9. 排错

**`mb list` 里看不到 `usage_page=0xFF01` 的设备**
- 该功能只适用于 Colorful / iGame 主板。用下面命令确认设备是否存在：
  ```bash
  for f in /sys/class/hidraw/hidraw*/device/uevent; do
    echo "-- $f"; grep -E 'HID_ID|HID_NAME' "$f"
  done
  ```
  应能看到 `HID_ID=0003:00002F4C:XXXXXXXX`。若没有，说明主板没有该灯控芯片。
- 若是不同 PID 的同类设备，`mb list` 在没有匹配时会把所有 hidraw 设备列出来，
  用其中的 VID:PID 配合 `--pid` 尝试。

**`Permission denied` 打开 `/dev/hidrawN`**
- 没重新登录，`i2c` 组未生效；或规则没触发（`sudo udevadm control --reload-rules && sudo udevadm trigger`）。

**发送成功但灯没变化**
- 先 `igame-led mb off --dry-run` 看报文；
- 试 `igame-led mb set ff0000` 看是否变色；
- 也可以 `igame-led mb raw 00 01 00 00 ...` 手工试报文。

**开机不自动关灯**
- 确认二进制在 `/usr/bin/igame-led`（即用 `PREFIX=/usr` 安装）；
- 某些系统对 udev `RUN+=` 长期运行的程序有限制，此时用 systemd 服务：
  `sudo systemctl enable --now igame-led.service`。

**查看 udev 是否命中**

```bash
sudo udevadm test /sys/class/hidraw/hidrawX
```

---

## 10. 安全说明

- 程序只写一条 65 字节的 HID 输出报文，且只针对 `VID 0x2F4C` 的设备。
- udev 规则里对 `i2c` 组的授权文件也可按要求精简（本工具实际只需要 `hidraw` 那条）。
