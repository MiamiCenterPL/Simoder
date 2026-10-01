namespace Simoder.DevBridge.Core;

/// <summary>Validates startup targets before any external process is touched.</summary>
public static class LaunchTargetValidator
{
    /// <summary>Validates and normalizes a requested target.</summary>
    /// <exception cref="ArgumentException">Thrown when required names are missing or inconsistent.</exception>
    public static LaunchTarget Validate(LaunchTarget target)
    {
        ArgumentNullException.ThrowIfNull(target);
        var region = Normalize(target.RegionName);
        var city = Normalize(target.CityName);

        if (target.Mode == LaunchMode.Continue)
        {
            if (region is not null || city is not null)
            {
                throw new ArgumentException("Continue mode does not accept regionName or cityName.");
            }

            return new LaunchTarget(LaunchMode.Continue);
        }

        if (region is null || city is null)
        {
            throw new ArgumentException("Play mode requires non-empty regionName and cityName.");
        }

        if (region.Length > 128 || city.Length > 128)
        {
            throw new ArgumentException("Region and city names must not exceed 128 characters.");
        }

        return new LaunchTarget(LaunchMode.Play, region, city);
    }

    /// <summary>Trims a user-visible name and converts empty values to null.</summary>
    private static string? Normalize(string? value)
    {
        var normalized = value?.Trim();
        return string.IsNullOrEmpty(normalized) ? null : normalized;
    }
}
