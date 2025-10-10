#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
创建一个简单的测试图像，包含几个模拟星点
"""
import numpy as np
import cv2

def create_test_star_image():
    # 创建一个黑色背景
    img = np.zeros((400, 400), dtype=np.uint8)
    
    # 添加一些模拟星点（高斯分布）
    stars = [
        (100, 100, 15),  # 位置(x,y)和亮度
        (200, 150, 20),
        (300, 200, 12),
        (150, 300, 18),
        (250, 80, 10)
    ]
    
    for x, y, brightness in stars:
        # 创建高斯核
        kernel_size = 15
        kernel = np.zeros((kernel_size, kernel_size))
        center = kernel_size // 2
        
        for i in range(kernel_size):
            for j in range(kernel_size):
                dist = np.sqrt((i - center)**2 + (j - center)**2)
                kernel[i, j] = brightness * np.exp(-(dist**2) / (2 * 3**2))
        
        # 将核添加到图像上
        y_start = max(0, y - center)
        y_end = min(img.shape[0], y + center + 1)
        x_start = max(0, x - center)
        x_end = min(img.shape[1], x + center + 1)
        
        k_y_start = max(0, center - y)
        k_y_end = k_y_start + (y_end - y_start)
        k_x_start = max(0, center - x)
        k_x_end = k_x_start + (x_end - x_start)
        
        img[y_start:y_end, x_start:x_end] = np.maximum(
            img[y_start:y_end, x_start:x_end],
            kernel[k_y_start:k_y_end, k_x_start:k_x_end].astype(np.uint8)
        )
    
    # 添加一些噪声
    noise = np.random.normal(0, 2, img.shape).astype(np.int16)
    img = np.clip(img.astype(np.int16) + noise, 0, 255).astype(np.uint8)
    
    return img

if __name__ == "__main__":
    test_img = create_test_star_image()
    cv2.imwrite("test_stars.png", test_img)
    print("测试图像已创建: test_stars.png")
