SELECT json_agg(ActiveKeymembers) FROM (SELECT members_member."number", members_member.name, profile_memberprofile.nick, byro_shackspace_shackprofile.ssh_public_key
	FROM members_member
	INNER JOIN byro_shackspace_shackprofile ON members_member.id = byro_shackspace_shackprofile.member_id
	INNER JOIN profile_memberprofile ON members_member.id = profile_memberprofile.member_id
	WHERE byro_shackspace_shackprofile.is_keyholder
	AND TRIM(BOTH FROM byro_shackspace_shackprofile.ssh_public_key) != ''
	AND 1 = (SELECT MIN(1) AS INTEGER
			FROM members_membership
			WHERE members_member.id = members_membership.member_id
			AND (public.members_membership."end" IS NULL 
				OR "start" >= NOW()::date AND "end" <= NOW()::date
				)
		)
) AS ActiveKeymembers