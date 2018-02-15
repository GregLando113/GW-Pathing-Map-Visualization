#include "Viewer.h"

#include <imgui.h>
#include "imgui_impl_sdl_gl2.h"

void DebugWait()
{
#ifdef _DEBUG
	while (!IsDebuggerPresent())
	{
		Sleep(100);
	}
#endif
}

Viewer::Viewer() {
	window = nullptr;
	trapezoids_ = std::vector<PathingMapTrapezoid>();
	max_plane_ = 1;
	mouse_down_ = false;
	refresh_ = false;
	scale_ = 0.0001;
	translate_ = Point2d();
	wireframe_ = false;
	circles_ = false;
	mapdata_ = nullptr;
	mapdatacount_ = 0;

	int n_vertices = 50;
	for (int i = 0; i < 50; ++i) {
		float angle = i * (2 * static_cast<float>(M_PI) / n_vertices);
		float x = std::cos(angle);
		float y = std::sin(angle);
		circle_vertices_.push_back(Point2d(x, y));
	}
	circle_vertices_.push_back(circle_vertices_[0]);

	circle_sizes_.push_back(300); // nearby aka hos
	circle_sizes_.push_back(322);
	circle_sizes_.push_back(366);
	circle_sizes_.push_back(1085); // spellcast
	circle_sizes_.push_back(2500); // spirit range
	circle_sizes_.push_back(5000); // compass
}

void Viewer::InitializeWindow() {
	SDL_Init(SDL_INIT_VIDEO);

	RECT r;
	GetWindowRect(GetDesktopWindow(),&r);
	window = SDL_CreateWindow("Guild Wars Pathing Visualizer",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		r.right * 0.75, r.bottom * 0.75,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

	SDL_GL_CreateContext(window);
	SDL_GL_SetSwapInterval(1);

	ImGui::CreateContext();
	ImGui_ImplSdlGL2_Init(window);

	glDisable(GL_LIGHTING);
	glDisable(GL_DEPTH_TEST);

	Resize(DEFAULT_WIDTH, DEFAULT_HEIGHT);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	Resize(r.right * 0.75, r.bottom * 0.75);
}

void Viewer::Resize(int width, int height) {
	width_ = width;
	height_ = height;
	printf("resizing to %d %d\n", width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	ratio_ = (double)width / height;
	glScaled(1, ratio_, 1);

	refresh_ = true;
}

void Viewer::LoadMapData(const char* file)
{
	//DebugWait();

	FILE* fh = fopen("mapinfo.csv", "r");
	if (!fh) {
		printf("Mapdata.csv fopen error!\n");
		return;
	}

	// Figure out how many map tables we need
	mapdatacount_ = 0;
	int c;
	do {
		c = fgetc(fh);
		if (c == '\n')
			mapdatacount_++;
	} while (c != EOF);
	mapdata_ = new GWMapData[mapdatacount_];
	fseek(fh, 0, SEEK_SET);

	unsigned idx = 0;
	char buffer[100];
	for(fgets(buffer, 100, fh); !feof(fh); fgets(buffer, 100, fh), ++idx) {
		char* seeker = buffer;

		// get mapid
		mapdata_[idx].mapid = strtoul(seeker, nullptr, 0);
		seeker = strchr(seeker, ',');
		seeker++;

		// get map name
		{
			unsigned namecount = 0;
			for (; *seeker != ',' && namecount < 0x100 - 1; seeker++, namecount++) {
				mapdata_[idx].name[namecount] = *seeker;
			}
			mapdata_[idx].name[namecount] = '\0';
			seeker++;
		}

		// get mapfile
		mapdata_[idx].mapfile = strtoul(seeker, nullptr, 0);
		seeker = strchr(seeker, ',');
		seeker++;

		// get spawn x
		mapdata_[idx].spawnx = atof(seeker);
		seeker = strchr(seeker, ',');
		seeker++;
		
		// get spawn y
		mapdata_[idx].spawny = atof(seeker);
		seeker = strchr(seeker, ',');
		seeker++;

		mapdata_[idx].selected = false;
		mapdata_[idx].visible = true;
	}
	

}

DWORD WINAPI PmapExtractor(LPVOID ab)
{
	LPSTR argbuffer = (LPSTR)ab;
	STARTUPINFOA pmapBuildInfo = {};
	PROCESS_INFORMATION pmapPI = {};
	if (!CreateProcessA(NULL, argbuffer, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &pmapBuildInfo, &pmapPI))
		printf("error in create process, err %d\n", GetLastError());
	WaitForSingleObject(pmapPI.hProcess, INFINITE);
	return 0;
}

void Viewer::Execute() {
	bool quit = false;
	while (!quit) {
		SDL_Delay(1);
		// event handling
		SDL_Event e;
		while (SDL_PollEvent(&e) != 0) {

			auto& io = ImGui::GetIO();
			ImGui_ImplSdlGL2_ProcessEvent(&e);

			if (io.WantCaptureMouse || io.WantTextInput)
				continue;

			switch (e.type) {
			case SDL_QUIT:
				quit = true;
				break;
			case SDL_MOUSEBUTTONDOWN:
				HandleMouseDownEvent(e.button);
				break;
			case SDL_MOUSEBUTTONUP:
				HandleMouseUpEvent(e.button);
				break;
			case SDL_MOUSEMOTION:
				HandleMouseMoveEvent(e.motion);
				break;
			case SDL_MOUSEWHEEL:
				HandleMouseWheelEvent(e.wheel);
				break;
			case SDL_WINDOWEVENT:
				HandleWindowEvent(e.window);
				break;
			case SDL_KEYDOWN:
				HandleKeyDownEvent(e.key);
				break;
			case SDL_KEYUP:
				HandleKeyUpEvent(e.key);
				break;
			default:
				break;
			}
		}

		

		//if (refresh_) {
			RenderPMap();
			refresh_ = false;
		//}

		ImGui_ImplSdlGL2_NewFrame(window);

		// PMAP downloader
		static bool   download_prompt = GetFileAttributesA("PMAPs") == INVALID_FILE_ATTRIBUTES;
		static HANDLE download_thread = 0;
		if (download_prompt || download_thread) {
			if (download_thread) {
				if (WaitForSingleObject(download_thread, 0) == WAIT_OBJECT_0) {
					download_thread = 0;
				}
				else {
					ImGui::Begin("waitForDownload", nullptr, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar);
					ImGui::Text("Please wait for extraction...");
					ImGui::End();
					goto endRender;
				}
			}

			if (ImGui::BeginPopupModal("pmapDownloadPrompt", &download_prompt, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
				static char argbuffer[MAX_PATH + 20] = "pmap.exe -e \"";
				static char* writer = argbuffer + strlen(argbuffer);
				ImGui::Text("PMAP directory not found,\nplease input your datfile and hit run.");
				ImGui::InputText("datfile path", writer, MAX_PATH);
				if (ImGui::Button("Go")) {
					strcat(argbuffer, "\"");
					download_thread = CreateThread(0, 0, PmapExtractor, argbuffer, 0, 0);
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}

			static bool pmap_download = download_prompt;
			if (pmap_download) {
				download_prompt = true;
				ImGui::OpenPopup("pmapDownloadPrompt");
				pmap_download = false;
			}
		}
		else {

			ImGui::SetNextWindowSizeConstraints(ImVec2(400, 100), ImVec2(400, -1));
			if (ImGui::Begin("Map List")) {
				ImGui::Columns(4, NULL, false);

				ImGui::SetColumnWidth(-1, 1);
				ImGui::Text("");
				for (unsigned i = 0; i < mapdatacount_; ++i) {
					ImGui::PushID(i);
					if (ImGui::Selectable("##Load", &mapdata_[i].selected, ImGuiSelectableFlags_SpanAllColumns)) {
						SetPMap(mapdata_[i].mapfile);
					}
					ImGui::PopID();
				}

				ImGui::NextColumn();
				ImGui::SetColumnWidth(-1, 40);
				ImGui::Text("ID");
				for (unsigned i = 0; i < mapdatacount_; ++i) {
					ImGui::Text("%d", mapdata_[i].mapid);
				}

				ImGui::NextColumn();
				ImGui::SetColumnWidth(-1, 200);
				ImGui::Text("Name");
				for (unsigned i = 0; i < mapdatacount_; ++i) {
					ImGui::Text(mapdata_[i].name);
				}

				ImGui::NextColumn();
				ImGui::SetColumnWidth(-1, 80);
				ImGui::Text("Map File");
				for (unsigned i = 0; i < mapdatacount_; ++i) {
					ImGui::Text("%d", mapdata_[i].mapfile);
				}
			}
			ImGui::End();

		}
		

	endRender:
		ImGui::Render();
		SDL_GL_SwapWindow(window);

		SDL_Delay(1000 / 60);
	}
}

void Viewer::SetPMap(unsigned mapfileid) {

	TCHAR filename[MAX_PATH];
	_stprintf_s(filename, TEXT("PMAPs\\MAP %010u.pmap"), mapfileid);
	PathingMap pmap(mapfileid);
	if (!pmap.Open(filename)) {
		printf("ERROR loading pmap %d\n", mapfileid);
	}
	trapezoids_ = pmap.GetPathingData();

	max_plane_ = 1;
	for (size_t i = 0; i < trapezoids_.size(); ++i) {
		if (max_plane_ < trapezoids_[i].Plane) {
			max_plane_ = trapezoids_[i].Plane;
		}
	}
	refresh_ = true;
}

void Viewer::RenderPMap() {
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glViewport(0, 0, width_, height_);
	glScaled(scale_, scale_, 1);
	glPushMatrix();
	glTranslated(translate_.x(), translate_.y(), 0);

	if (wireframe_) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); // wireframe
	} else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // full quads
	}

	glColor3f(1, 0, 0);
	glBegin(GL_QUADS);
	for (size_t i = 0; i < trapezoids_.size(); ++i) {
		float c = (float)trapezoids_[i].Plane / max_plane_;
		glColor3f(c, 1 - c, 0);
		glVertex2f(trapezoids_[i].XTL, trapezoids_[i].YT);
		glVertex2f(trapezoids_[i].XTR, trapezoids_[i].YT);
		glVertex2f(trapezoids_[i].XBR, trapezoids_[i].YB);
		glVertex2f(trapezoids_[i].XBL, trapezoids_[i].YB);
	}
	glEnd();

	if (circles_) {
		glPopMatrix();
		glTranslated(center_.x(), center_.y(), 0.0);
		glColor3f(1.0f, 1.0f, 1.0f);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // full quads

		glBegin(GL_POINT);
		glVertex2f(0.0f, 0.0f);
		glEnd();

		for (size_t j = 0; j < circle_sizes_.size(); ++j) {
			glPushMatrix();
			glScaled(circle_sizes_[j], circle_sizes_[j], 0.0);

			glBegin(GL_LINE_STRIP);
			for (size_t i = 0; i < circle_vertices_.size(); ++i) {
				glVertex2d(circle_vertices_[i].x(), circle_vertices_[i].y());
			}
			glEnd();
			glPopMatrix();
		}

		
		//glBegin(GL_TRIANGLES);
		//glVertex2f(center_.x(), center_.y());
		//glVertex2f(center_.x() + 500, center_.y() - 500);
		//glVertex2f(center_.x() - 500, center_.y() - 500);
		//glEnd();
	}

	glPopMatrix();
	
}

void Viewer::Close() {
	ImGui_ImplSdlGL2_Shutdown();
	ImGui::DestroyContext();

	SDL_DestroyWindow(window);
	SDL_Quit();
}

void Viewer::HandleMouseDownEvent(SDL_MouseButtonEvent button) {
	if (button.button == SDL_BUTTON_LEFT) {
		mouse_down_ = true;
	}
}

void Viewer::HandleMouseUpEvent(SDL_MouseButtonEvent button) {
	if (button.button == SDL_BUTTON_LEFT) {
		mouse_down_ = false;
	}
}

void Viewer::HandleMouseMoveEvent(SDL_MouseMotionEvent motion) {
	if (mouse_down_) {
		Point2d diff = Point2d(motion.xrel, -motion.yrel);
		diff.x() /= width_; // remap from [0, WIDTH] to [0, 1]
		diff.y() /= height_; // remap from [0, HEIGHT] to [0, 1]
		diff.y() /= ratio_; // adjust for window aspect ratio
		diff *= 2; // remap from [0, 1]^2 to [0, 2]^2 (screen space is [-1, 1] so range has to be 2
		diff /= scale_; // remap for scale

		translate_ += diff;
		
		refresh_ = true;
	}

	{
		center_ = Point2d(motion.x, motion.y);
		center_.x() /= width_; // remap from [0, Width] to [0, 1]
		center_.y() /= -height_; // remap from [0, Height] to [0, 1]
		center_.y() += 0.5f;
		center_.x() -= 0.5f;
		center_.y() /= ratio_; // adjust for window aspect ratio
		center_ *= 2; // remap from [0, 1]^2 to [0, 2]^2
		center_ /= scale_;

		refresh_ = true;
	}
}

void Viewer::HandleMouseWheelEvent(SDL_MouseWheelEvent wheel) {
	if (wheel.y > 0) {
		scale_ *= 1.25;
	} else {
		scale_ *= 0.8;
	}
	refresh_ = true;
}

void Viewer::HandleWindowEvent(SDL_WindowEvent window) {
	switch (window.event) {
	case SDL_WINDOWEVENT_RESIZED:
		Resize(window.data1, window.data2);
		break;
	default:
		break;
	}
}

void Viewer::HandleKeyDownEvent(SDL_KeyboardEvent keyboard) {
	switch (keyboard.keysym.sym) {
	default:
		break;
	}
}

void Viewer::HandleKeyUpEvent(SDL_KeyboardEvent keyboard) {
	switch (keyboard.keysym.sym) {
	case SDLK_SPACE:
		wireframe_ = !wireframe_;
		refresh_ = true;
		break;
	case SDLK_c:
		circles_ = !circles_;
		refresh_ = true;
		break;
	default:
		break;
	}
}
