"""Quick source/release contracts for the 0.12 streaming-stability hotfix."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
manager = (root / 'src/world/chunks/Chunk_Manager.cpp').read_text()
player = (root / 'src/entities/player/Player.cpp').read_text()
game = (root / 'src/core/game/Game.cpp').read_text()
hud = (root / 'src/hud/Hud.cpp').read_text()
window = (root / 'src/world/chunks/Chunk_Stream_Window.h').read_text()
release = (root / 'documentation/history_version.md').read_text()
archived = (root / 'documentation/history_version.md').read_text()
assert 'Chunk_Stream_Window::wanted(' in manager
assert 'Chunk_Stream_Window::vertical_half(' in manager
assert 'std::push_heap' in manager and 'std::pop_heap' in manager
assert 'm_chunks.size() > 20000' in manager
assert 'std::vector<Far> to_unload' not in manager
assert 'waiting_for_terrain' in player
assert 'is_chunk_loaded' in player
assert 'process_rss_mib()' in game and 'Process RAM' in hud
assert 'бета' in release.lower() and 'стабильность' in release.lower()
assert '0.11' in archived
assert (root / 'VERSION').read_text().strip() in {'0.12', '0.12.1', '0.12.2', '0.13'}
print('OptiCraft 0.12 source/release contracts: PASS')
