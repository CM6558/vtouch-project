// 测试 vtouchws 强制断开旧连接
"use strict";

console.log("[TEST] 启动第一个连接...");
var ws1 = new WebSocket("ws://127.0.0.1:27183");
ws1.on("open", function() {
    console.log("[TEST] 连接1 已打开");
    ws1.send("ping");
});
ws1.on("message", function(msg) {
    console.log("[TEST] 连接1 收到: " + msg);
});
ws1.on("closed", function() {
    console.log("[TEST] 连接1 已关闭");
});
ws1.on("failure", function(err) {
    console.log("[TEST] 连接1 失败: " + err);
});

// 等待 1 秒后启动第二个连接（应该踢掉第一个）
setTimeout(function() {
    console.log("[TEST] 启动第二个连接（应该踢掉连接1）...");
    var ws2 = new WebSocket("ws://127.0.0.1:27183");
    ws2.on("open", function() {
        console.log("[TEST] 连接2 已打开");
        ws2.send("ping");
        
        setTimeout(function() {
            console.log("[TEST] 测试完成，退出");
            ws2.close();
            exit();
        }, 1000);
    });
    ws2.on("message", function(msg) {
        console.log("[TEST] 连接2 收到: " + msg);
    });
    ws2.on("closed", function() {
        console.log("[TEST] 连接2 已关闭");
    });
    ws2.on("failure", function(err) {
        console.log("[TEST] 连接2 失败: " + err);
    });
}, 1000);
