import subprocess
import os

def convert_font(font_path, size, output_dir, ranges="0x20-0x7F"):
    """
    使用 Python 脚本调用本地 lv_font_conv 工具
    """
    font_name = os.path.splitext(os.path.basename(font_path))[0]
    output_file = os.path.join(output_dir, f"lv_font_{font_name}_{size}.c")
    
    cmd = [
        "lv_font_conv",
        "--font", font_path,
        "-r", ranges,
        "--size", str(size),
        "-o", output_file,
        "--format", "lvgl",
        "--bpp", "4"
    ]
    
    print(f"正在转换字体: {font_name} 为 {size}px...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode == 0:
        print(f"转换成功! 已生成: {output_file}")
    else:
        print(f"转换失败: {result.stderr}")

# 使用示例
if __name__ == "__main__":
    # 你的字体文件、目标大小和存放目录
    FONT_FILE = "./fonts/SimSun.ttf"
    OUTPUT_DIR = "./output"
    
    # 包含常用 ASCII 和 常用汉字范围
    CHINESE_RANGE = "0x20-0x7F,0x4E00-0x9FA5" 
    
    # 批量生成不同大小的字体
    for size in [16, 24, 32]:
        convert_font(FONT_FILE, size, OUTPUT_DIR, ranges=CHINESE_RANGE)
