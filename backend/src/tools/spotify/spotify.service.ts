import { Injectable } from '@nestjs/common';
import axios from 'axios';
import { AppConfigService } from '../../config/app-config.service';
import { AppLoggerService } from '../../logging/logger.module';
import { SpotifyResolutionError } from '../../errors/agent-errors';

@Injectable()
export class SpotifyService {
  private cachedToken: string | null = null;
  private tokenExpiresAtMs = 0;

  constructor(
    private readonly config: AppConfigService,
    private readonly logger: AppLoggerService,
  ) {}

  async resolveSpotifyUri(query: string): Promise<string | null> {
    if (!query) {
      return null;
    }
    const token = await this.getAccessToken();
    if (!token) {
      return null;
    }
    const type = 'track,playlist,artist';
    const url = `https://api.spotify.com/v1/search?q=${encodeURIComponent(query)}&type=${type}&limit=1`;
    try {
      const resp = await axios.get(url, { headers: { Authorization: `Bearer ${token}` } });
      if (resp.data.tracks?.items.length > 0) {
        return resp.data.tracks.items[0].uri;
      }
      if (resp.data.artists?.items.length > 0) {
        return resp.data.artists.items[0].uri;
      }
      if (resp.data.playlists?.items.length > 0) {
        return resp.data.playlists.items[0].uri;
      }
      return null;
    } catch (error) {
      this.logger.log('SPOTIFY', 'error', `API error: ${error}`);
      throw new SpotifyResolutionError(query, error);
    }
  }

  private async getAccessToken(): Promise<string | null> {
    const now = Date.now();
    if (this.cachedToken && now < this.tokenExpiresAtMs) {
      return this.cachedToken;
    }

    const clientId = this.config.spotifyClientId;
    const clientSecret = this.config.spotifyClientSecret;
    if (!clientId || !clientSecret) {
      this.logger.log('SPOTIFY', 'warn', 'Spotify credentials not configured');
      return null;
    }

    const creds = Buffer.from(`${clientId}:${clientSecret}`).toString('base64');
    try {
      const resp = await axios.post(
        'https://accounts.spotify.com/api/token',
        'grant_type=client_credentials',
        {
          headers: {
            Authorization: `Basic ${creds}`,
            'Content-Type': 'application/x-www-form-urlencoded',
          },
        },
      );
      this.cachedToken = resp.data.access_token;
      const expiresInSec = resp.data.expires_in ?? 3600;
      this.tokenExpiresAtMs = now + expiresInSec * 1000 - 60_000;
      return this.cachedToken;
    } catch (error) {
      this.logger.log('SPOTIFY', 'error', `Token error: ${error}`);
      return null;
    }
  }
}
