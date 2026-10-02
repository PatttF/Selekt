import re

def main():
    with open('src/main.cpp', 'r') as f:
        data = f.read()

    start_idx = data.find('static void MainLoopStep()')
    if start_idx == -1: return
    brace_open_idx = data.find('{', start_idx)
    brace_close_idx = -1

    brace_count = 0
    for i in range(brace_open_idx, len(data)):
        if data[i] == '{': brace_count += 1
        elif data[i] == '}':
            brace_count -= 1
            if brace_count == 0:
                brace_close_idx = i
                break

    if brace_close_idx != -1:
        clean_loop_body = """{
    ImGuiIO& io = ImGui::GetIO();
    if (g_EglDisplay == EGL_NO_DISPLAY || !g_selektApp)
        return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    renderFrame(*g_selektApp);

    ImGui::Render();

    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.04f, 0.04f, 0.07f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(g_EglDisplay, g_EglSurface);
}"""
        data = data[:brace_open_idx] + clean_loop_body + data[brace_close_idx+1:]

    # Fluid UI Tweaks
    data = re.sub(
        r'bool isCT\s*=\s*app\.midi\.outName\(\)\.find\("Circuit Tracks"\) != std::string::npos;',
        r'bool isCT = true; // FORCED TRUE FOR ANDROID UI',
        data
    )
    data = re.sub(
        r'bool isCT\s*=\s*app\.midi\.curOut\(\) >= 0 &&\s*app\.midi\.outName\(\)\.find\("Circuit Tracks"\) != std::string::npos;',
        r'bool isCT = true; // FORCED TRUE FOR ANDROID UI',
        data
    )
    data = re.sub(
        r'bool isCT2\s*=\s*app\.midi\.curOut\(\) >= 0 &&\s*app\.midi\.outName\(\)\.find\("Circuit Tracks"\) != std::string::npos;',
        r'bool isCT2 = true; // FORCED TRUE FOR ANDROID UI',
        data
    )

    data = re.sub(r'if \(rowH > 44\) rowH = 44;', r'// if (rowH > 44) rowH = 44;', data)
    data = re.sub(r'if \(padS > 72\) padS = 72;', r'// if (padS > 72) padS = 72;', data)
    data = re.sub(r'float leftW = W \* 0\.3f;', r'float leftW = W * 0.35f;', data)

    data = re.sub(r'float hdrH = 56;', r'float hdrH = 56 * app.scale;', data)
    data = re.sub(r'const float btnH = hdrH - 6\.f;', r'const float btnH = hdrH - (6.f * app.scale);', data)
    data = re.sub(r'float rowY = 3\.f;', r'float rowY = 3.f * app.scale;', data)
    data = re.sub(r'float bx = 8\.f;', r'float bx = 8.f * app.scale;', data)
    data = re.sub(r'const float dh\s*=\s*36\.f;', r'const float dh = 36.f * app.scale;', data)
    data = re.sub(r'const float dpad\s*=\s*6\.f;', r'const float dpad = 6.f * app.scale;', data)
    data = re.sub(r'float bw = 44\.f \* app\.scale;', r'float bw = 64.f * app.scale;', data)
    data = re.sub(r'float btnW = 44\.f \* app\.scale, btnH = 40\.f \* app\.scale;', r'float btnW = 64.f * app.scale, btnH = 48.f * app.scale;', data)

    with open('src/main.cpp', 'w') as f:
        f.write(data)

if __name__ == '__main__':
    main()
