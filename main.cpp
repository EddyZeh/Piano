#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <vector>
#include <cmath>
#include <iostream>

struct PianoKey {
	sf::RectangleShape shape;
	bool isPressed = false;
	int soundIndex = -1;
};

sf::SoundBuffer generateSineWave(float frequency, float durationSeconds = 0.3f, unsigned sampleRate = 44100) {
	const int sampleCount = static_cast<int>(durationSeconds * sampleRate);
	std::vector<std::int16_t> samples(sampleCount);

	const float amplitude = 30000.0f;
	const float twoPiF = 2 * 3.14159265f * frequency;

	for (int i = 0; i < sampleCount; ++i) {
		float time = i / static_cast<float>(sampleRate);
		samples[i] = static_cast<std::int16_t>(amplitude * std::sin(twoPiF * time));
	}

	std::vector <sf::SoundChannel> channelMap = std::vector<sf::SoundChannel>{

			sf::SoundChannel::FrontLeft,
	};

	sf::SoundBuffer buffer;
	if (!buffer.loadFromSamples(samples.data(), sampleCount, 1, sampleRate, channelMap)) {
		std::cerr << "Failed to create sound buffer.\n";
	}

	return buffer;
}

int main() {
	sf::RenderWindow window(sf::VideoMode({ 800, 300 }), "SFML Piano", sf::Style::Close);
	window.setFramerateLimit(60);

	const int whiteKeyWidth = 60;
	const int whiteKeyHeight = 200;
	const int blackKeyWidth = 40;
	const int blackKeyHeight = 120;

	const int numWhiteKeys = 12;

	std::vector<PianoKey> whiteKeys;
	std::vector<PianoKey> blackKeys;

	std::vector<int> blackKeyPosition = { 0, 1, 3, 4, 5 };

	float frequencyTable[12][9];
	for (int i = 0; i < 12; ++i) {
		for (int j = 0; j < 9; ++j) {
			frequencyTable[i][j] = 440.0f * pow(2.0f, ((j * 12 + i) - 57) / 12.0f);
		}
	}

	std::vector<sf::SoundBuffer> soundBuffers(12);
	for (int i = 0; i < 12; ++i) {
		soundBuffers[i] = generateSineWave(frequencyTable[i][4]);
	}
	sf::Sound currentSound(soundBuffers[0]);

	std::vector<int> blackSemitoneIndicies = { 1, 3, 6, 8, 10 };
	std::vector<sf::SoundBuffer> blackBuffer(5);
	for (int i = 0; i < 5; ++i) {
		blackBuffer[i] = generateSineWave(frequencyTable[blackSemitoneIndicies[i]][4]);
	}

	for (int i = 0; i < numWhiteKeys; ++i) {
		PianoKey key;
		key.shape.setSize(sf::Vector2f({ whiteKeyWidth, whiteKeyHeight }));
		key.shape.setFillColor(sf::Color::White);
		key.shape.setOutlineColor(sf::Color::Black);
		key.shape.setOutlineThickness(2);
		key.shape.setPosition({ static_cast<float>(i * whiteKeyWidth), 50.0f });
		key.soundIndex = i;
		whiteKeys.push_back(key);
	}

	for (int i = 0; i < blackKeyPosition.size(); ++i) {
		int index = blackKeyPosition[i];
		PianoKey key;
		key.shape.setSize(sf::Vector2f({ blackKeyWidth, blackKeyHeight }));
		key.shape.setFillColor(sf::Color::Black);
		key.shape.setPosition({ static_cast<int>((index + 1) * whiteKeyWidth) - blackKeyWidth / 2.0f, 50.0f });
		key.soundIndex = i;
		blackKeys.push_back(key);
	}

	bool blackKeyPressed = false;

	while (window.isOpen()) {
		
		while (const std::optional event = window.pollEvent()) {
			if (event->is<sf::Event::Closed>())
				window.close();
			else if (const auto* mousePressed = event->getIf<sf::Event::MouseButtonPressed>()) {
				if (window.hasFocus()) {

					if (mousePressed->button == sf::Mouse::Button::Left) {
						sf::Vector2f mousePos = static_cast<sf::Vector2f>(mousePressed->position);
						for (auto& key : blackKeys) {
							if (key.shape.getGlobalBounds().contains(mousePos)) {
								key.isPressed = true;
								blackKeyPressed = true;
								currentSound = sf::Sound(blackBuffer[key.soundIndex]);
								currentSound.play();
							}
						}
						if (!blackKeyPressed) {
							for (auto& key : whiteKeys) {
								if (key.shape.getGlobalBounds().contains(mousePos)) {
									key.isPressed = true;
									if (key.soundIndex != -1) {
										currentSound =  sf::Sound(soundBuffers[key.soundIndex]);
										currentSound.play();
									}
								}
							}
						}
					}
				}
			}
			else if (event->is<sf::Event::MouseButtonReleased>()) {
				for (auto& key : blackKeys) key.isPressed = false;
				for (auto& key : whiteKeys) key.isPressed = false;
				blackKeyPressed = false;
			}

		}

		window.clear(sf::Color(30, 30, 30));
		for (auto& key : whiteKeys) {
			if (key.isPressed) {
				key.shape.setFillColor(sf::Color(200, 200, 255));
			}
			else {
				key.shape.setFillColor(sf::Color::White);
			}
			window.draw(key.shape);
		}

		for (auto& key : blackKeys) {
			if (key.isPressed) {
				key.shape.setFillColor(sf::Color(70, 70, 255));
			}
			else {
				key.shape.setFillColor(sf::Color::Black);
			}
			window.draw(key.shape);
		}
		window.display();
	}
	return 0;
}