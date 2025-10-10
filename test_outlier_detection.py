#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
异常点检测功能测试脚本

测试QT端和VUE端的异常点检测算法
模拟对焦数据，包含一些异常点，验证算法能否正确识别
"""

import numpy as np
import matplotlib.pyplot as plt
import json

def generate_test_data():
    """生成包含异常点的测试数据"""
    # 生成正常的二次曲线数据
    positions = np.linspace(1000, 5000, 20)
    # 二次函数: y = 0.0001 * (x - 3000)^2 + 2.0
    normal_hfr = 0.0001 * (positions - 3000) ** 2 + 2.0
    
    # 添加一些噪声
    noise = np.random.normal(0, 0.1, len(positions))
    normal_hfr += noise
    
    # 添加异常点
    outlier_indices = [3, 8, 15]  # 选择几个位置作为异常点
    outlier_hfr = [8.5, 12.3, 6.7]  # 明显偏离正常值的HFR
    
    # 创建完整的数据集
    all_positions = list(positions)
    all_hfr = list(normal_hfr)
    
    for i, idx in enumerate(outlier_indices):
        all_hfr[idx] = outlier_hfr[i]
    
    return list(zip(all_positions, all_hfr))

def test_iqr_outlier_detection(data_points):
    """测试IQR异常点检测方法"""
    hfr_values = [point[1] for point in data_points]
    
    # 排序
    sorted_hfr = sorted(hfr_values)
    
    # 计算四分位数
    n = len(sorted_hfr)
    q1 = sorted_hfr[n // 4]
    q3 = sorted_hfr[3 * n // 4]
    iqr = q3 - q1
    
    # 定义异常值边界（使用2倍IQR）
    lower_bound = q1 - 2.0 * iqr
    upper_bound = q3 + 2.0 * iqr
    
    # 过滤异常值
    clean_data = []
    outliers = []
    
    for point in data_points:
        if lower_bound <= point[1] <= upper_bound:
            clean_data.append(point)
        else:
            outliers.append(point)
    
    return clean_data, outliers

def test_residual_outlier_detection(data_points):
    """测试基于残差的异常点检测方法"""
    if len(data_points) < 4:
        return data_points, []
    
    # 先进行IQR初步清理
    preliminary_clean, _ = test_iqr_outlier_detection(data_points)
    
    if len(preliminary_clean) < 3:
        return data_points, []
    
    # 对初步清理的数据进行二次拟合
    positions = [point[0] for point in preliminary_clean]
    hfr_values = [point[1] for point in preliminary_clean]
    
    # 标准化坐标
    min_pos = min(positions)
    x_values = [pos - min_pos for pos in positions]
    
    # 构建最小二乘法正规方程组
    sum_x4 = sum(x**4 for x in x_values)
    sum_x3 = sum(x**3 for x in x_values)
    sum_x2 = sum(x**2 for x in x_values)
    sum_x = sum(x_values)
    sum_1 = len(x_values)
    
    sum_x2y = sum(x**2 * y for x, y in zip(x_values, hfr_values))
    sum_xy = sum(x * y for x, y in zip(x_values, hfr_values))
    sum_y = sum(hfr_values)
    
    # 构建系数矩阵
    matrix = [
        [sum_x4, sum_x3, sum_x2],
        [sum_x3, sum_x2, sum_x],
        [sum_x2, sum_x, sum_1]
    ]
    
    constants = [sum_x2y, sum_xy, sum_y]
    
    # 求解线性方程组（简化版）
    try:
        # 使用numpy求解
        import numpy as np
        coefficients = np.linalg.solve(matrix, constants)
        a, b, c = coefficients
    except:
        return data_points, []
    
    # 计算所有数据点到拟合曲线的残差
    residuals = []
    for point in data_points:
        x = point[0] - min_pos
        predicted_y = a * x**2 + b * x + c
        residual = abs(point[1] - predicted_y)
        residuals.append(residual)
    
    # 计算残差的统计信息
    sorted_residuals = sorted(residuals)
    n = len(sorted_residuals)
    q1 = sorted_residuals[n // 4]
    q3 = sorted_residuals[3 * n // 4]
    iqr = q3 - q1
    threshold = q3 + 2.0 * iqr
    
    # 识别异常点
    clean_data = []
    outliers = []
    
    for i, point in enumerate(data_points):
        if residuals[i] <= threshold:
            clean_data.append(point)
        else:
            outliers.append(point)
    
    return clean_data, outliers

def visualize_results(original_data, clean_data, outliers, method_name):
    """可视化测试结果"""
    plt.figure(figsize=(12, 8))
    
    # 原始数据点
    orig_pos, orig_hfr = zip(*original_data)
    plt.scatter(orig_pos, orig_hfr, color='blue', alpha=0.6, label='原始数据点', s=50)
    
    # 清理后的数据点
    clean_pos, clean_hfr = zip(*clean_data)
    plt.scatter(clean_pos, clean_hfr, color='green', alpha=0.8, label='正常数据点', s=50)
    
    # 异常点
    if outliers:
        outlier_pos, outlier_hfr = zip(*outliers)
        plt.scatter(outlier_pos, outlier_hfr, color='red', alpha=0.8, label='检测到的异常点', s=100, marker='x')
    
    plt.xlabel('对焦位置')
    plt.ylabel('HFR值')
    plt.title(f'{method_name} - 异常点检测结果')
    plt.legend()
    plt.grid(True, alpha=0.3)
    
    # 保存图片
    plt.savefig(f'/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/test_outlier_{method_name.lower().replace(" ", "_")}.png', 
                dpi=150, bbox_inches='tight')
    plt.show()

def main():
    """主测试函数"""
    print("开始异常点检测功能测试...")
    
    # 生成测试数据
    test_data = generate_test_data()
    print(f"生成了 {len(test_data)} 个测试数据点")
    
    # 显示原始数据
    print("\n原始数据:")
    for i, (pos, hfr) in enumerate(test_data):
        print(f"  点 {i+1}: 位置={pos:.1f}, HFR={hfr:.3f}")
    
    # 测试IQR方法
    print("\n=== 测试IQR异常点检测方法 ===")
    clean_data_iqr, outliers_iqr = test_iqr_outlier_detection(test_data)
    print(f"检测结果: {len(clean_data_iqr)} 个正常点, {len(outliers_iqr)} 个异常点")
    
    if outliers_iqr:
        print("检测到的异常点:")
        for pos, hfr in outliers_iqr:
            print(f"  位置={pos:.1f}, HFR={hfr:.3f}")
    
    # 测试残差方法
    print("\n=== 测试残差异常点检测方法 ===")
    clean_data_residual, outliers_residual = test_residual_outlier_detection(test_data)
    print(f"检测结果: {len(clean_data_residual)} 个正常点, {len(outliers_residual)} 个异常点")
    
    if outliers_residual:
        print("检测到的异常点:")
        for pos, hfr in outliers_residual:
            print(f"  位置={pos:.1f}, HFR={hfr:.3f}")
    
    # 可视化结果
    try:
        visualize_results(test_data, clean_data_iqr, outliers_iqr, "IQR方法")
        visualize_results(test_data, clean_data_residual, outliers_residual, "残差方法")
        print("\n测试结果已保存为图片文件")
    except Exception as e:
        print(f"可视化失败: {e}")
    
    # 生成VUE端测试数据
    vue_test_data = {
        "original_data": test_data,
        "iqr_clean_data": clean_data_iqr,
        "iqr_outliers": outliers_iqr,
        "residual_clean_data": clean_data_residual,
        "residual_outliers": outliers_residual
    }
    
    with open('/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/vue_test_data.json', 'w', encoding='utf-8') as f:
        json.dump(vue_test_data, f, ensure_ascii=False, indent=2)
    
    print("\nVUE端测试数据已保存到 vue_test_data.json")
    print("异常点检测功能测试完成！")

if __name__ == "__main__":
    main()


