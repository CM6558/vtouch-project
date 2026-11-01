package com.vtouch.editor;

/**
 * 测试服务（兼容入口名，与 ChipService 同一实现）：胶囊 + 测试台。
 * 保留此名是为了兼容早前文档里的触发命令：
 *   su -c "am start-foreground-service -n com.vtouch.editor/.RenderService --es mode all|quick|render"
 */
public class RenderService extends ProbeService {
}
