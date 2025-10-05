package main

import (
	"encoding/xml"
	"fmt"
	"io"
	"log"
	"net/http"
	"strconv"
	"time"
)

const (
	baseURL   = "https://api.met.no/weatherapi"
	userAgent = "liustatus/1.0"

	// Prague coordinates.
	lat      = 50.08804
	lon      = 14.42076
	altitude = 202
)

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

type Weatherdata struct {
	XMLName xml.Name `xml:"weatherdata"`
	Product Product  `xml:"product"`
}

type Product struct {
	Times []Time `xml:"time"`
}

type Time struct {
	From     string   `xml:"from,attr"`
	To       string   `xml:"to,attr"`
	Location Location `xml:"location"`
}

type Location struct {
	Temperature *Temperature `xml:"temperature"`
}

type Temperature struct {
	Unit  string `xml:"unit,attr"`
	Value string `xml:"value,attr"`
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// WeatherFetcher handles weather data retrieval.
type WeatherFetcher struct {
	client *http.Client
}

// NewWeatherFetcher creates a new weather fetcher instance.
func NewWeatherFetcher() *WeatherFetcher {
	return &WeatherFetcher{
		client: &http.Client{Timeout: 30 * time.Second},
	}
}

// fetchWeather retrieves the current temperature from the API.
func (w *WeatherFetcher) fetchWeather() (string, error) {
	url := fmt.Sprintf(
		"%s/locationforecast/2.0/classic?lat=%.5f&lon=%.5f&altitude=%d",
		baseURL, lat, lon, altitude)

	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return "", err
	}

	req.Header.Set("User-Agent", userAgent)

	resp, err := w.client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("API returned status %d", resp.StatusCode)
	}

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", err
	}

	var weatherData Weatherdata
	if err := xml.Unmarshal(body, &weatherData); err != nil {
		return "", err
	}

	now := time.Now().UTC()
	for _, t := range weatherData.Product.Times {
		toTime, err := time.Parse("2006-01-02T15:04:05Z", t.To)
		if err != nil || toTime.Before(now) {
			continue
		}
		if t.Location.Temperature != nil {
			temp, err := strconv.ParseFloat(t.Location.Temperature.Value, 64)
			if err != nil {
				continue
			}
			return fmt.Sprintf("%dﾟ", int(temp)), nil
		}
	}

	return "", fmt.Errorf("no usable temperature data found")
}

// update fetches new weather data and returns it.
func (w *WeatherFetcher) update() string {
	temp, err := w.fetchWeather()
	if err != nil {
		log.Printf("Error fetching weather: %v", err)
	}
	return temp
}

// Run runs as a goroutine to periodically fetch weather data.
func (w *WeatherFetcher) Run(interval time.Duration, output chan<- string) {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	output <- w.update()
	for range ticker.C {
		output <- w.update()
	}
}
